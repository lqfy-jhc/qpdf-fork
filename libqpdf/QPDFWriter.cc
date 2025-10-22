#include <qpdf/qpdf-config.h> // include early for large file support

#include <qpdf/QPDFWriter_private.hh>

#include <qpdf/MD5.hh>
#include <qpdf/Pl_AES_PDF.hh>
#include <qpdf/Pl_Flate.hh>
#include <qpdf/Pl_MD5.hh>
#include <qpdf/Pl_PNGFilter.hh>
#include <qpdf/Pl_RC4.hh>
#include <qpdf/Pl_StdioFile.hh>
#include <qpdf/QIntC.hh>
#include <qpdf/QPDFObjectHandle_private.hh>
#include <qpdf/QPDFObject_private.hh>
#include <qpdf/QPDF_private.hh>
#include <qpdf/QTC.hh>
#include <qpdf/QUtil.hh>
#include <qpdf/RC4.hh>
#include <qpdf/Util.hh>

#include <algorithm>
#include <concepts>
#include <cstdlib>
#include <stdexcept>
#include <tuple>

using namespace std::literals;
using namespace qpdf;

using Encryption = impl::Doc::Encryption;
using Config = Writer::Config;

QPDFWriter::ProgressReporter::~ProgressReporter() // NOLINT (modernize-use-equals-default)
{
    // Must be explicit and not inline -- see QPDF_DLL_CLASS in README-maintainer
}

QPDFWriter::FunctionProgressReporter::FunctionProgressReporter(std::function<void(int)> handler) :
    handler(handler)
{
}

QPDFWriter::FunctionProgressReporter::~FunctionProgressReporter() // NOLINT
                                                                  // (modernize-use-equals-default)
{
    // Must be explicit and not inline -- see QPDF_DLL_CLASS in README-maintainer
}

void
QPDFWriter::FunctionProgressReporter::reportProgress(int progress)
{
    handler(progress);
}

namespace
{
    class Pl_stack
    {
        // A pipeline Popper is normally returned by Pl_stack::activate, or, if necessary, a
        // reference to a Popper instance can be passed into activate. When the Popper goes out of
        // scope, the pipeline stack is popped. This causes finish to be called on the current
        // pipeline and the pipeline stack to be popped until the top of stack is a previous active
        // top of stack and restores the pipeline to that point. It deletes any pipelines that it
        // pops.
        class Popper
        {
            friend class Pl_stack;

          public:
            Popper() = default;
            Popper(Popper const&) = delete;
            Popper(Popper&& other) noexcept
            {
                // For MSVC, default pops the stack
                if (this != &other) {
                    stack = other.stack;
                    stack_id = other.stack_id;
                    other.stack = nullptr;
                    other.stack_id = 0;
                };
            }
            Popper& operator=(Popper const&) = delete;
            Popper&
            operator=(Popper&& other) noexcept
            {
                // For MSVC, default pops the stack
                if (this != &other) {
                    stack = other.stack;
                    stack_id = other.stack_id;
                    other.stack = nullptr;
                    other.stack_id = 0;
                };
                return *this;
            }

            ~Popper();

            // Manually pop pipeline from the pipeline stack.
            void pop();

          private:
            Popper(Pl_stack& stack) :
                stack(&stack)
            {
            }

            Pl_stack* stack{nullptr};
            unsigned long stack_id{0};
        };

      public:
        Pl_stack(pl::Count*& top) :
            top(top)
        {
        }

        Popper
        popper()
        {
            return {*this};
        }

        void
        initialize(Pipeline* p)
        {
            auto c = std::make_unique<pl::Count>(++last_id, p);
            top = c.get();
            stack.emplace_back(std::move(c));
        }

        Popper
        activate(std::string& str)
        {
            Popper pp{*this};
            activate(pp, str);
            return pp;
        }

        void
        activate(Popper& pp, std::string& str)
        {
            activate(pp, false, &str, nullptr);
        }

        void
        activate(Popper& pp, std::unique_ptr<Pipeline> next)
        {
            count_buffer.clear();
            activate(pp, false, &count_buffer, std::move(next));
        }

        Popper
        activate(
            bool discard = false,
            std::string* str = nullptr,
            std::unique_ptr<Pipeline> next = nullptr)
        {
            Popper pp{*this};
            activate(pp, discard, str, std::move(next));
            return pp;
        }

        void
        activate(
            Popper& pp,
            bool discard = false,
            std::string* str = nullptr,
            std::unique_ptr<Pipeline> next = nullptr)
        {
            std::unique_ptr<pl::Count> c;
            if (next) {
                c = std::make_unique<pl::Count>(++last_id, count_buffer, std::move(next));
            } else if (discard) {
                c = std::make_unique<pl::Count>(++last_id, nullptr);
            } else if (!str) {
                c = std::make_unique<pl::Count>(++last_id, top);
            } else {
                c = std::make_unique<pl::Count>(++last_id, *str);
            }
            pp.stack_id = last_id;
            top = c.get();
            stack.emplace_back(std::move(c));
        }
        void
        activate_md5(Popper& pp)
        {
            qpdf_assert_debug(!md5_pipeline);
            qpdf_assert_debug(md5_id == 0);
            qpdf_assert_debug(top->getCount() == 0);
            md5_pipeline = std::make_unique<Pl_MD5>("qpdf md5", top);
            md5_pipeline->persistAcrossFinish(true);
            // Special case code in pop clears m->md5_pipeline upon deletion.
            auto c = std::make_unique<pl::Count>(++last_id, md5_pipeline.get());
            pp.stack_id = last_id;
            md5_id = last_id;
            top = c.get();
            stack.emplace_back(std::move(c));
        }

        // Return the hex digest and disable the MD5 pipeline.
        std::string
        hex_digest()
        {
            qpdf_assert_debug(md5_pipeline);
            auto digest = md5_pipeline->getHexDigest();
            md5_pipeline->enable(false);
            return digest;
        }

        void
        clear_buffer()
        {
            count_buffer.clear();
        }

      private:
        void
        pop(unsigned long stack_id)
        {
            if (!stack_id) {
                return;
            }
            qpdf_assert_debug(stack.size() >= 2);
            top->finish();
            qpdf_assert_debug(stack.back().get() == top);
            // It used to be possible for this assertion to fail if writeLinearized exits by
            // exception when deterministic ID. There are no longer any cases in which two
            // dynamically allocated pipeline Popper objects ever exist at the same time, so the
            // assertion will fail if they get popped out of order from automatic destruction.
            qpdf_assert_debug(top->id() == stack_id);
            if (stack_id == md5_id) {
                md5_pipeline = nullptr;
                md5_id = 0;
            }
            stack.pop_back();
            top = stack.back().get();
        }

        std::vector<std::unique_ptr<pl::Count>> stack;
        pl::Count*& top;
        std::unique_ptr<Pl_MD5> md5_pipeline{nullptr};
        unsigned long last_id{0};
        unsigned long md5_id{0};
        std::string count_buffer;
    };
} // namespace

Pl_stack::Popper::~Popper()
{
    if (stack) {
        stack->pop(stack_id);
    }
}

void
Pl_stack::Popper::pop()
{
    if (stack) {
        stack->pop(stack_id);
    }
    stack_id = 0;
    stack = nullptr;
}

namespace qpdf::impl
{
    // Writer class is restricted to QPDFWriter so that only it can call certain methods.
    class Writer: protected Doc::Common
    {
      public:
        // flags used by unparseObject
        static int const f_stream = 1 << 0;
        static int const f_filtered = 1 << 1;
        static int const f_in_ostream = 1 << 2;
        static int const f_hex_string = 1 << 3;
        static int const f_no_encryption = 1 << 4;

        enum trailer_e { t_normal, t_lin_first, t_lin_second };

        Writer() = delete;
        Writer(Writer const&) = delete;
        Writer(Writer&&) = delete;
        Writer& operator=(Writer const&) = delete;
        Writer& operator=(Writer&&) = delete;
        ~Writer()
        {
            if (file && close_file) {
                fclose(file);
            }
            delete output_buffer;
        }
        Writer(QPDF& qpdf, QPDFWriter& w) :
            Common(qpdf.doc()),
            lin(qpdf.doc().linearization()),
            cfg(true),
            root_og(qpdf.getRoot().indirect() ? qpdf.getRoot().id_gen() : QPDFObjGen(-1, 0)),
            pipeline_stack(pipeline)
        {
        }

        void write();
        std::map<QPDFObjGen, QPDFXRefEntry> getWrittenXRefTable();
        void setMinimumPDFVersion(std::string const& version, int extension_level = 0);
        void copyEncryptionParameters(QPDF&);
        void doWriteSetup();
        void prepareFileForWrite();

        void disableIncompatibleEncryption(int major, int minor, int extension_level);
        void interpretR3EncryptionParameters(
            bool allow_accessibility,
            bool allow_extract,
            bool allow_assemble,
            bool allow_annotate_and_form,
            bool allow_form_filling,
            bool allow_modify_other,
            qpdf_r3_print_e print,
            qpdf_r3_modify_e modify);
        void setEncryptionParameters(char const* user_password, char const* owner_password);
        void setEncryptionMinimumVersion();
        void parseVersion(std::string const& version, int& major, int& minor) const;
        int compareVersions(int major1, int minor1, int major2, int minor2) const;
        void generateID(bool encrypted);
        std::string getOriginalID1();
        void initializeTables(size_t extra = 0);
        void preserveObjectStreams();
        void generateObjectStreams();
        void initializeSpecialStreams();
        void enqueue(QPDFObjectHandle const& object);
        void enqueueObjectsStandard();
        void enqueueObjectsPCLm();
        void enqueuePart(std::vector<QPDFObjectHandle>& part);
        void assignCompressedObjectNumbers(QPDFObjGen og);
        Dictionary trimmed_trailer();

        // Returns tuple<filter, compress_stream, is_root_metadata>
        std::tuple<const bool, const bool, const bool>
        will_filter_stream(QPDFObjectHandle stream, std::string* stream_data);

        // Test whether stream would be filtered if it were written.
        bool will_filter_stream(QPDFObjectHandle stream);
        unsigned int bytesNeeded(long long n);
        void writeBinary(unsigned long long val, unsigned int bytes);
        Writer& write(std::string_view str);
        Writer& write(size_t count, char c);
        Writer& write(std::integral auto val);
        Writer& write_name(std::string const& str);
        Writer& write_string(std::string const& str, bool force_binary = false);
        Writer& write_encrypted(std::string_view str);

        template <typename... Args>
        Writer& write_qdf(Args&&... args);
        template <typename... Args>
        Writer& write_no_qdf(Args&&... args);
        void writeObjectStreamOffsets(std::vector<qpdf_offset_t>& offsets, int first_obj);
        void writeObjectStream(QPDFObjectHandle object);
        void writeObject(QPDFObjectHandle object, int object_stream_index = -1);
        void writeTrailer(
            trailer_e which,
            int size,
            bool xref_stream,
            qpdf_offset_t prev,
            int linearization_pass);
        void unparseObject(
            QPDFObjectHandle object,
            size_t level,
            int flags,
            // for stream dictionaries
            size_t stream_length = 0,
            bool compress = false);
        void unparseChild(QPDFObjectHandle const& child, size_t level, int flags);
        int openObject(int objid = 0);
        void closeObject(int objid);
        void writeStandard();
        void writeLinearized();
        void writeEncryptionDictionary();
        void writeHeader();
        void writeHintStream(int hint_id);
        qpdf_offset_t writeXRefTable(trailer_e which, int first, int last, int size);
        qpdf_offset_t writeXRefTable(
            trailer_e which,
            int first,
            int last,
            int size,
            // for linearization
            qpdf_offset_t prev,
            bool suppress_offsets,
            int hint_id,
            qpdf_offset_t hint_offset,
            qpdf_offset_t hint_length,
            int linearization_pass);
        qpdf_offset_t writeXRefStream(
            int objid,
            int max_id,
            qpdf_offset_t max_offset,
            trailer_e which,
            int first,
            int last,
            int size);
        qpdf_offset_t writeXRefStream(
            int objid,
            int max_id,
            qpdf_offset_t max_offset,
            trailer_e which,
            int first,
            int last,
            int size,
            // for linearization
            qpdf_offset_t prev,
            int hint_id,
            qpdf_offset_t hint_offset,
            qpdf_offset_t hint_length,
            bool skip_compression,
            int linearization_pass);

        void setDataKey(int objid);
        void indicateProgress(bool decrement, bool finished);
        size_t calculateXrefStreamPadding(qpdf_offset_t xref_bytes);

        void adjustAESStreamLength(size_t& length);
        void computeDeterministicIDData();

      protected:
        Doc::Linearization& lin;

        qpdf::Writer::Config cfg;

        QPDFObjGen root_og{-1, 0};
        char const* filename{"unspecified"};
        FILE* file{nullptr};
        bool close_file{false};
        std::unique_ptr<Pl_Buffer> buffer_pipeline{nullptr};
        Buffer* output_buffer{nullptr};

        std::unique_ptr<QPDF::Doc::Encryption> encryption;
        std::string encryption_key;

        std::string id1; // for /ID key of
        std::string id2; // trailer dictionary
        std::string final_pdf_version;
        int final_extension_level{0};
        std::string min_pdf_version;
        int min_extension_level{0};
        int encryption_dict_objid{0};
        std::string cur_data_key;
        std::unique_ptr<Pipeline> file_pl;
        qpdf::pl::Count* pipeline{nullptr};
        std::vector<QPDFObjectHandle> object_queue;
        size_t object_queue_front{0};
        QPDFWriter::ObjTable obj;
        QPDFWriter::NewObjTable new_obj;
        int next_objid{1};
        int cur_stream_length_id{0};
        size_t cur_stream_length{0};
        bool added_newline{false};
        size_t max_ostream_index{0};
        std::set<QPDFObjGen> normalized_streams;
        std::map<QPDFObjGen, int> page_object_to_seq;
        std::map<QPDFObjGen, int> contents_to_page_seq;
        std::map<int, std::vector<QPDFObjGen>> object_stream_to_objects;
        Pl_stack pipeline_stack;
        std::string deterministic_id_data;
        bool did_write_setup{false};

        // For progress reporting
        std::shared_ptr<QPDFWriter::ProgressReporter> progress_reporter;
        int events_expected{0};
        int events_seen{0};
        int next_progress_report{0};
    }; // class qpdf::impl::Writer

} // namespace qpdf::impl

class QPDFWriter::Members: impl::Writer
{
    friend class QPDFWriter;
    friend class qpdf::Writer;

  public:
    Members(QPDFWriter& w, QPDF& qpdf) :
        impl::Writer(qpdf, w)
    {
    }
};

qpdf::Writer::Writer(QPDF& qpdf, Config cfg) :
    QPDFWriter(qpdf)
{
    m->cfg = cfg;
}
QPDFWriter::QPDFWriter(QPDF& pdf) :
    m(std::make_shared<Members>(*this, pdf))
{
}

QPDFWriter::QPDFWriter(QPDF& pdf, char const* filename) :
    m(std::make_shared<Members>(*this, pdf))
{
    setOutputFilename(filename);
}

QPDFWriter::QPDFWriter(QPDF& pdf, char const* description, FILE* file, bool close_file) :
    m(std::make_shared<Members>(*this, pdf))
{
    setOutputFile(description, file, close_file);
}

void
QPDFWriter::setOutputFilename(char const* filename)
{
    char const* description = filename;
    FILE* f = nullptr;
    bool close_file = false;
    if (filename == nullptr) {
        description = "standard output";
        f = stdout;
        QUtil::binary_stdout();
    } else {
        f = QUtil::safe_fopen(filename, "wb+");
        close_file = true;
    }
    setOutputFile(description, f, close_file);
}

void
QPDFWriter::setOutputFile(char const* description, FILE* file, bool close_file)
{
    m->filename = description;
    m->file = file;
    m->close_file = close_file;
    m->file_pl = std::make_unique<Pl_StdioFile>("qpdf output", file);
    m->pipeline_stack.initialize(m->file_pl.get());
}

void
QPDFWriter::setOutputMemory()
{
    m->filename = "memory buffer";
    m->buffer_pipeline = std::make_unique<Pl_Buffer>("qpdf output");
    m->pipeline_stack.initialize(m->buffer_pipeline.get());
}

Buffer*
QPDFWriter::getBuffer()
{
    Buffer* result = m->output_buffer;
    m->output_buffer = nullptr;
    return result;
}

std::shared_ptr<Buffer>
QPDFWriter::getBufferSharedPointer()
{
    return std::shared_ptr<Buffer>(getBuffer());
}

void
QPDFWriter::setOutputPipeline(Pipeline* p)
{
    m->filename = "custom pipeline";
    m->pipeline_stack.initialize(p);
}

void
QPDFWriter::setObjectStreamMode(qpdf_object_stream_e mode)
{
    m->cfg.object_streams(mode);
}

void
QPDFWriter::setStreamDataMode(qpdf_stream_data_e mode)
{
    m->cfg.stream_data(mode);
}

Config&
Config::stream_data(qpdf_stream_data_e mode)
{
    switch (mode) {
    case qpdf_s_uncompress:
        decode_level(std::max(qpdf_dl_generalized, decode_level_));
        compress_streams(false);
        return *this;

    case qpdf_s_preserve:
        decode_level(qpdf_dl_none);
        compress_streams(false);
        return *this;

    case qpdf_s_compress:
        decode_level(std::max(qpdf_dl_generalized, decode_level_));
        compress_streams(true);
    }
    return *this;
}

void
QPDFWriter::setCompressStreams(bool val)
{
    m->cfg.compress_streams(val);
}

Config&
Config::compress_streams(bool val)
{
    if (pclm_) {
        usage("compress_streams cannot be set when pclm is set");
        return *this;
    }
    compress_streams_set_ = true;
    compress_streams_ = val;
    return *this;
}

void
QPDFWriter::setDecodeLevel(qpdf_stream_decode_level_e val)
{
    m->cfg.decode_level(val);
}

Config&
Config::decode_level(qpdf_stream_decode_level_e val)
{
    if (pclm_) {
        usage("stream_decode_level cannot be set when pclm is set");
        return *this;
    }
    decode_level_set_ = true;
    decode_level_ = val;
    return *this;
}

void
QPDFWriter::setRecompressFlate(bool val)
{
    m->cfg.recompress_flate(val);
}

void
QPDFWriter::setContentNormalization(bool val)
{
    m->cfg.normalize_content(val);
}

void
QPDFWriter::setQDFMode(bool val)
{
    m->cfg.qdf(val);
}

Config&
Config::qdf(bool val)
{
    if (pclm_ || linearize_) {
        usage("qdf cannot be set when linearize or pclm are set");
    }
    if (preserve_encryption_) {
        usage("preserve_encryption cannot be set when qdf is set");
    }
    qdf_ = val;
    if (val) {
        if (!normalize_content_set_) {
            normalize_content(true);
        }
        if (!compress_streams_set_) {
            compress_streams(false);
        }
        if (!decode_level_set_) {
            decode_level(qpdf_dl_generalized);
        }
        preserve_encryption_ = false;
        // Generate indirect stream lengths for qdf mode since fix-qdf uses them for storing
        // recomputed stream length data. Certain streams such as object streams, xref streams, and
        // hint streams always get direct stream lengths.
        direct_stream_lengths_ = false;
    }
    return *this;
}

void
QPDFWriter::setPreserveUnreferencedObjects(bool val)
{
    m->cfg.preserve_unreferenced(val);
}

void
QPDFWriter::setNewlineBeforeEndstream(bool val)
{
    m->cfg.newline_before_endstream(val);
}

void
QPDFWriter::setMinimumPDFVersion(std::string const& version, int extension_level)
{
    m->setMinimumPDFVersion(version, extension_level);
}

void
impl::Writer::setMinimumPDFVersion(std::string const& version, int extension_level)
{
    bool set_version = false;
    bool set_extension_level = false;
    if (min_pdf_version.empty()) {
        set_version = true;
        set_extension_level = true;
    } else {
        int old_major = 0;
        int old_minor = 0;
        int min_major = 0;
        int min_minor = 0;
        parseVersion(version, old_major, old_minor);
        parseVersion(min_pdf_version, min_major, min_minor);
        int compare = compareVersions(old_major, old_minor, min_major, min_minor);
        if (compare > 0) {
            QTC::TC("qpdf", "QPDFWriter increasing minimum version", extension_level == 0 ? 0 : 1);
            set_version = true;
            set_extension_level = true;
        } else if (compare == 0) {
            if (extension_level > min_extension_level) {
                set_extension_level = true;
            }
        }
    }

    if (set_version) {
        min_pdf_version = version;
    }
    if (set_extension_level) {
        min_extension_level = extension_level;
    }
}

void
QPDFWriter::setMinimumPDFVersion(PDFVersion const& v)
{
    std::string version;
    int extension_level;
    v.getVersion(version, extension_level);
    setMinimumPDFVersion(version, extension_level);
}

void
QPDFWriter::forcePDFVersion(std::string const& version, int extension_level)
{
    m->cfg.forced_pdf_version(version, extension_level);
}

void
QPDFWriter::setExtraHeaderText(std::string const& text)
{
    m->cfg.extra_header_text(text);
}

Config&
Config::extra_header_text(std::string const& val)
{
    extra_header_text_ = val;
    if (!extra_header_text_.empty() && extra_header_text_.back() != '\n') {
        extra_header_text_ += "\n";
    } else {
        QTC::TC("qpdf", "QPDFWriter extra header text no newline");
    }
    return *this;
}

void
QPDFWriter::setStaticID(bool val)
{
    m->cfg.static_id(val);
}

void
QPDFWriter::setDeterministicID(bool val)
{
    m->cfg.deterministic_id(val);
}

void
QPDFWriter::setStaticAesIV(bool val)
{
    if (val) {
        Pl_AES_PDF::useStaticIV();
    }
}

void
QPDFWriter::setSuppressOriginalObjectIDs(bool val)
{
    m->cfg.no_original_object_ids(val);
}

void
QPDFWriter::setPreserveEncryption(bool val)
{
    m->cfg.preserve_encryption(val);
}

void
QPDFWriter::setLinearization(bool val)
{
    m->cfg.linearize(val);
}

Config&
Config::linearize(bool val)
{
    if (pclm_ || qdf_) {
        usage("linearize cannot be set when qdf or pclm are set");
        return *this;
    }
    linearize_ = val;
    return *this;
}

void
QPDFWriter::setLinearizationPass1Filename(std::string const& filename)
{
    m->cfg.linearize_pass1(filename);
}

void
QPDFWriter::setPCLm(bool val)
{
    m->cfg.pclm(val);
}

Config&
Config::pclm(bool val)
{
    if (decode_level_set_ || compress_streams_set_ || linearize_) {
        usage(
            "pclm cannot be set when stream_decode_level, compress_streams, linearize or qdf are "
            "set");
        return *this;
    }
    pclm_ = val;
    if (val) {
        decode_level_ = qpdf_dl_none;
        compress_streams_ = false;
        linearize_ = false;
    }

    return *this;
}

void
QPDFWriter::setR2EncryptionParametersInsecure(
    char const* user_password,
    char const* owner_password,
    bool allow_print,
    bool allow_modify,
    bool allow_extract,
    bool allow_annotate)
{
    m->encryption = std::make_unique<Encryption>(1, 2, 5, true);
    if (!allow_print) {
        m->encryption->setP(3, false);
    }
    if (!allow_modify) {
        m->encryption->setP(4, false);
    }
    if (!allow_extract) {
        m->encryption->setP(5, false);
    }
    if (!allow_annotate) {
        m->encryption->setP(6, false);
    }
    m->setEncryptionParameters(user_password, owner_password);
}

void
QPDFWriter::setR3EncryptionParametersInsecure(
    char const* user_password,
    char const* owner_password,
    bool allow_accessibility,
    bool allow_extract,
    bool allow_assemble,
    bool allow_annotate_and_form,
    bool allow_form_filling,
    bool allow_modify_other,
    qpdf_r3_print_e print)
{
    m->encryption = std::make_unique<Encryption>(2, 3, 16, true);
    m->interpretR3EncryptionParameters(
        allow_accessibility,
        allow_extract,
        allow_assemble,
        allow_annotate_and_form,
        allow_form_filling,
        allow_modify_other,
        print,
        qpdf_r3m_all);
    m->setEncryptionParameters(user_password, owner_password);
}

void
QPDFWriter::setR4EncryptionParametersInsecure(
    char const* user_password,
    char const* owner_password,
    bool allow_accessibility,
    bool allow_extract,
    bool allow_assemble,
    bool allow_annotate_and_form,
    bool allow_form_filling,
    bool allow_modify_other,
    qpdf_r3_print_e print,
    bool encrypt_metadata,
    bool use_aes)
{
    m->encryption = std::make_unique<Encryption>(4, 4, 16, encrypt_metadata);
    m->cfg.encrypt_use_aes(use_aes);
    m->interpretR3EncryptionParameters(
        allow_accessibility,
        allow_extract,
        allow_assemble,
        allow_annotate_and_form,
        allow_form_filling,
        allow_modify_other,
        print,
        qpdf_r3m_all);
    m->setEncryptionParameters(user_password, owner_password);
}

void
QPDFWriter::setR5EncryptionParameters(
    char const* user_password,
    char const* owner_password,
    bool allow_accessibility,
    bool allow_extract,
    bool allow_assemble,
    bool allow_annotate_and_form,
    bool allow_form_filling,
    bool allow_modify_other,
    qpdf_r3_print_e print,
    bool encrypt_metadata)
{
    m->encryption = std::make_unique<Encryption>(5, 5, 32, encrypt_metadata);
    m->cfg.encrypt_use_aes(true);
    m->interpretR3EncryptionParameters(
        allow_accessibility,
        allow_extract,
        allow_assemble,
        allow_annotate_and_form,
        allow_form_filling,
        allow_modify_other,
        print,
        qpdf_r3m_all);
    m->setEncryptionParameters(user_password, owner_password);
}

void
QPDFWriter::setR6EncryptionParameters(
    char const* user_password,
    char const* owner_password,
    bool allow_accessibility,
    bool allow_extract,
    bool allow_assemble,
    bool allow_annotate_and_form,
    bool allow_form_filling,
    bool allow_modify_other,
    qpdf_r3_print_e print,
    bool encrypt_metadata)
{
    m->encryption = std::make_unique<Encryption>(5, 6, 32, encrypt_metadata);
    m->interpretR3EncryptionParameters(
        allow_accessibility,
        allow_extract,
        allow_assemble,
        allow_annotate_and_form,
        allow_form_filling,
        allow_modify_other,
        print,
        qpdf_r3m_all);
    m->cfg.encrypt_use_aes(true);
    m->setEncryptionParameters(user_password, owner_password);
}

void
impl::Writer::interpretR3EncryptionParameters(
    bool allow_accessibility,
    bool allow_extract,
    bool allow_assemble,
    bool allow_annotate_and_form,
    bool allow_form_filling,
    bool allow_modify_other,
    qpdf_r3_print_e print,
    qpdf_r3_modify_e modify)
{
    // Acrobat 5 security options:

    // Checkboxes:
    //   Enable Content Access for the Visually Impaired
    //   Allow Content Copying and Extraction

    // Allowed changes menu:
    //   None
    //   Only Document Assembly
    //   Only Form Field Fill-in or Signing
    //   Comment Authoring, Form Field Fill-in or Signing
    //   General Editing, Comment and Form Field Authoring

    // Allowed printing menu:
    //   None
    //   Low Resolution
    //   Full printing

    // Meanings of bits in P when R >= 3
    //
    //  3: low-resolution printing
    //  4: document modification except as controlled by 6, 9, and 11
    //  5: extraction
    //  6: add/modify annotations (comment), fill in forms
    //     if 4+6 are set, also allows modification of form fields
    //  9: fill in forms even if 6 is clear
    // 10: accessibility; ignored by readers, should always be set
    // 11: document assembly even if 4 is clear
    // 12: high-resolution printing
    if (!allow_accessibility && encryption->getR() <= 3) {
        // Bit 10 is deprecated and should always be set.  This used to mean accessibility.  There
        // is no way to disable accessibility with R > 3.
        encryption->setP(10, false);
    }
    if (!allow_extract) {
        encryption->setP(5, false);
    }

    switch (print) {
    case qpdf_r3p_none:
        encryption->setP(3, false); // any printing
        [[fallthrough]];
    case qpdf_r3p_low:
        encryption->setP(12, false); // high resolution printing
        [[fallthrough]];
    case qpdf_r3p_full:
        break;
        // no default so gcc warns for missing cases
    }

    // Modify options. The qpdf_r3_modify_e options control groups of bits and lack the full
    // flexibility of the spec. This is unfortunate, but it's been in the API for ages, and we're
    // stuck with it. See also allow checks below to control the bits individually.

    // NOT EXERCISED IN TEST SUITE
    switch (modify) {
    case qpdf_r3m_none:
        encryption->setP(11, false); // document assembly
        [[fallthrough]];
    case qpdf_r3m_assembly:
        encryption->setP(9, false); // filling in form fields
        [[fallthrough]];
    case qpdf_r3m_form:
        encryption->setP(6, false); // modify annotations, fill in form fields
        [[fallthrough]];
    case qpdf_r3m_annotate:
        encryption->setP(4, false); // other modifications
        [[fallthrough]];
    case qpdf_r3m_all:
        break;
        // no default so gcc warns for missing cases
    }
    // END NOT EXERCISED IN TEST SUITE

    if (!allow_assemble) {
        encryption->setP(11, false);
    }
    if (!allow_annotate_and_form) {
        encryption->setP(6, false);
    }
    if (!allow_form_filling) {
        encryption->setP(9, false);
    }
    if (!allow_modify_other) {
        encryption->setP(4, false);
    }
}

void
impl::Writer::setEncryptionParameters(char const* user_password, char const* owner_password)
{
    generateID(true);
    encryption->setId1(id1);
    encryption_key = encryption->compute_parameters(user_password, owner_password);
    setEncryptionMinimumVersion();
}

void
QPDFWriter::copyEncryptionParameters(QPDF& qpdf)
{
    m->copyEncryptionParameters(qpdf);
}

void
impl::Writer::copyEncryptionParameters(QPDF& qpdf)
{
    cfg.preserve_encryption(false);
    QPDFObjectHandle trailer = qpdf.getTrailer();
    if (trailer.hasKey("/Encrypt")) {
        generateID(true);
        id1 = trailer.getKey("/ID").getArrayItem(0).getStringValue();
        QPDFObjectHandle encrypt = trailer.getKey("/Encrypt");
        int V = encrypt.getKey("/V").getIntValueAsInt();
        int key_len = 5;
        if (V > 1) {
            key_len = encrypt.getKey("/Length").getIntValueAsInt() / 8;
        }
        const bool encrypt_metadata =
            encrypt.hasKey("/EncryptMetadata") && encrypt.getKey("/EncryptMetadata").isBool()
            ? encrypt.getKey("/EncryptMetadata").getBoolValue()
            : true;
        if (V >= 4) {
            // When copying encryption parameters, use AES even if the original file did not.
            // Acrobat doesn't create files with V >= 4 that don't use AES, and the logic of
            // figuring out whether AES is used or not is complicated with /StmF, /StrF, and /EFF
            // all potentially having different values.
            cfg.encrypt_use_aes(true);
        }
        QTC::TC("qpdf", "QPDFWriter copy encrypt metadata", encrypt_metadata ? 0 : 1);
        QTC::TC("qpdf", "QPDFWriter copy use_aes", cfg.encrypt_use_aes() ? 0 : 1);

        encryption = std::make_unique<Encryption>(
            V,
            encrypt.getKey("/R").getIntValueAsInt(),
            key_len,
            static_cast<int>(encrypt.getKey("/P").getIntValue()),
            encrypt.getKey("/O").getStringValue(),
            encrypt.getKey("/U").getStringValue(),
            V < 5 ? "" : encrypt.getKey("/OE").getStringValue(),
            V < 5 ? "" : encrypt.getKey("/UE").getStringValue(),
            V < 5 ? "" : encrypt.getKey("/Perms").getStringValue(),
            id1, // id1 == the other file's id1
            encrypt_metadata);
        encryption_key = V >= 5 ? qpdf.getEncryptionKey()
                                : encryption->compute_encryption_key(qpdf.getPaddedUserPassword());
        setEncryptionMinimumVersion();
    }
}

void
impl::Writer::disableIncompatibleEncryption(int major, int minor, int extension_level)
{
    if (!encryption) {
        return;
    }
    if (compareVersions(major, minor, 1, 3) < 0) {
        encryption = nullptr;
        return;
    }
    int V = encryption->getV();
    int R = encryption->getR();
    if (compareVersions(major, minor, 1, 4) < 0) {
        if (V > 1 || R > 2) {
            encryption = nullptr;
        }
    } else if (compareVersions(major, minor, 1, 5) < 0) {
        if (V > 2 || R > 3) {
            encryption = nullptr;
        }
    } else if (compareVersions(major, minor, 1, 6) < 0) {
        if (cfg.encrypt_use_aes()) {
            encryption = nullptr;
        }
    } else if (
        (compareVersions(major, minor, 1, 7) < 0) ||
        ((compareVersions(major, minor, 1, 7) == 0) && extension_level < 3)) {
        if (V >= 5 || R >= 5) {
            encryption = nullptr;
        }
    }

    if (!encryption) {
        QTC::TC("qpdf", "QPDFWriter forced version disabled encryption");
    }
}

void
impl::Writer::parseVersion(std::string const& version, int& major, int& minor) const
{
    major = QUtil::string_to_int(version.c_str());
    minor = 0;
    size_t p = version.find('.');
    if ((p != std::string::npos) && (version.length() > p)) {
        minor = QUtil::string_to_int(version.substr(p + 1).c_str());
    }
    std::string tmp = std::to_string(major) + "." + std::to_string(minor);
    if (tmp != version) {
        // The version number in the input is probably invalid. This happens with some files that
        // are designed to exercise bugs, such as files in the fuzzer corpus. Unfortunately
        // QPDFWriter doesn't have a way to give a warning, so we just ignore this case.
    }
}

int
impl::Writer::compareVersions(int major1, int minor1, int major2, int minor2) const
{
    if (major1 < major2) {
        return -1;
    }
    if (major1 > major2) {
        return 1;
    }
    if (minor1 < minor2) {
        return -1;
    }
    return minor1 > minor2 ? 1 : 0;
}

void
impl::Writer::setEncryptionMinimumVersion()
{
    auto const R = encryption->getR();
    if (R >= 6) {
        setMinimumPDFVersion("1.7", 8);
    } else if (R == 5) {
        setMinimumPDFVersion("1.7", 3);
    } else if (R == 4) {
        setMinimumPDFVersion(cfg.encrypt_use_aes() ? "1.6" : "1.5");
    } else if (R == 3) {
        setMinimumPDFVersion("1.4");
    } else {
        setMinimumPDFVersion("1.3");
    }
}

void
impl::Writer::setDataKey(int objid)
{
    if (encryption) {
        cur_data_key = QPDF::compute_data_key(
            encryption_key,
            objid,
            0,
            cfg.encrypt_use_aes(),
            encryption->getV(),
            encryption->getR());
    }
}

unsigned int
impl::Writer::bytesNeeded(long long n)
{
    unsigned int bytes = 0;
    while (n) {
        ++bytes;
        n >>= 8;
    }
    return bytes;
}

void
impl::Writer::writeBinary(unsigned long long val, unsigned int bytes)
{
    if (bytes > sizeof(unsigned long long)) {
        throw std::logic_error("QPDFWriter::writeBinary called with too many bytes");
    }
    unsigned char data[sizeof(unsigned long long)];
    for (unsigned int i = 0; i < bytes; ++i) {
        data[bytes - i - 1] = static_cast<unsigned char>(val & 0xff);
        val >>= 8;
    }
    pipeline->write(data, bytes);
}

impl::Writer&
impl::Writer::write(std::string_view str)
{
    pipeline->write(str);
    return *this;
}

impl::Writer&
impl::Writer::write(std::integral auto val)
{
    pipeline->write(std::to_string(val));
    return *this;
}

impl::Writer&
impl::Writer::write(size_t count, char c)
{
    pipeline->write(count, c);
    return *this;
}

impl::Writer&
impl::Writer::write_name(std::string const& str)
{
    pipeline->write(Name::normalize(str));
    return *this;
}

impl::Writer&
impl::Writer::write_string(std::string const& str, bool force_binary)
{
    pipeline->write(QPDF_String(str).unparse(force_binary));
    return *this;
}

template <typename... Args>
impl::Writer&
impl::Writer::write_qdf(Args&&... args)
{
    if (cfg.qdf()) {
        pipeline->write(std::forward<Args>(args)...);
    }
    return *this;
}

template <typename... Args>
impl::Writer&
impl::Writer::write_no_qdf(Args&&... args)
{
    if (!cfg.qdf()) {
        pipeline->write(std::forward<Args>(args)...);
    }
    return *this;
}

void
impl::Writer::adjustAESStreamLength(size_t& length)
{
    if (encryption && !cur_data_key.empty() && cfg.encrypt_use_aes()) {
        // Stream length will be padded with 1 to 16 bytes to end up as a multiple of 16.  It will
        // also be prepended by 16 bits of random data.
        length += 32 - (length & 0xf);
    }
}

impl::Writer&
impl::Writer::write_encrypted(std::string_view str)
{
    if (!(encryption && !cur_data_key.empty())) {
        write(str);
    } else if (cfg.encrypt_use_aes()) {
        write(pl::pipe<Pl_AES_PDF>(str, true, cur_data_key));
    } else {
        write(pl::pipe<Pl_RC4>(str, cur_data_key));
    }

    return *this;
}

void
impl::Writer::computeDeterministicIDData()
{
    if (!id2.empty()) {
        // Can't happen in the code
        throw std::logic_error(
            "Deterministic ID computation enabled after ID generation has already occurred.");
    }
    qpdf_assert_debug(deterministic_id_data.empty());
    deterministic_id_data = pipeline_stack.hex_digest();
}

int
impl::Writer::openObject(int objid)
{
    if (objid == 0) {
        objid = next_objid++;
    }
    new_obj[objid].xref = QPDFXRefEntry(pipeline->getCount());
    write(objid).write(" 0 obj\n");
    return objid;
}

void
impl::Writer::closeObject(int objid)
{
    // Write a newline before endobj as it makes the file easier to repair.
    write("\nendobj\n").write_qdf("\n");
    auto& no = new_obj[objid];
    no.length = pipeline->getCount() - no.xref.getOffset();
}

void
impl::Writer::assignCompressedObjectNumbers(QPDFObjGen og)
{
    int objid = og.getObj();
    if (og.getGen() != 0 || !object_stream_to_objects.contains(objid)) {
        // This is not an object stream.
        return;
    }

    // Reserve numbers for the objects that belong to this object stream.
    for (auto const& iter: object_stream_to_objects[objid]) {
        obj[iter].renumber = next_objid++;
    }
}

void
impl::Writer::enqueue(QPDFObjectHandle const& object)
{
    if (object.indirect()) {
        util::assertion(
            // This owner check can only be done for indirect objects. It is possible for a direct
            // object to have an owning QPDF that is from another file if a direct QPDFObjectHandle
            // from one file was insert into another file without copying. Doing that is safe even
            // if the original QPDF gets destroyed, which just disconnects the QPDFObjectHandle from
            // its owner.
            object.qpdf() == &qpdf,
            "QPDFObjectHandle from different QPDF found while writing.  "
            "Use QPDF::copyForeignObject to add objects from another file." //
        );

        if (cfg.qdf() && object.isStreamOfType("/XRef")) {
            // As a special case, do not output any extraneous XRef streams in QDF mode. Doing so
            // will confuse fix-qdf, which expects to see only one XRef stream at the end of the
            // file. This case can occur when creating a QDF from a file with object streams when
            // preserving unreferenced objects since the old cross reference streams are not
            // actually referenced by object number.
            return;
        }

        QPDFObjGen og = object.getObjGen();
        auto& o = obj[og];

        if (o.renumber == 0) {
            if (o.object_stream > 0) {
                // This is in an object stream.  Don't process it here.  Instead, enqueue the object
                // stream.  Object streams always have generation 0.
                // Detect loops by storing invalid object ID -1, which will get overwritten later.
                o.renumber = -1;
                enqueue(qpdf.getObject(o.object_stream, 0));
            } else {
                object_queue.emplace_back(object);
                o.renumber = next_objid++;

                if (og.getGen() == 0 && object_stream_to_objects.contains(og.getObj())) {
                    // For linearized files, uncompressed objects go at end, and we take care of
                    // assigning numbers to them elsewhere.
                    if (!cfg.linearize()) {
                        assignCompressedObjectNumbers(og);
                    }
                } else if (!cfg.direct_stream_lengths() && object.isStream()) {
                    // reserve next object ID for length
                    ++next_objid;
                }
            }
        }
        return;
    }

    if (cfg.linearize()) {
        return;
    }

    if (Array array = object) {
        for (auto& item: array) {
            enqueue(item);
        }
        return;
    }

    for (auto const& item: Dictionary(object)) {
        if (!item.second.null()) {
            enqueue(item.second);
        }
    }
}

void
impl::Writer::unparseChild(QPDFObjectHandle const& child, size_t level, int flags)
{
    if (!cfg.linearize()) {
        enqueue(child);
    }
    if (child.indirect()) {
        write(obj[child].renumber).write(" 0 R");
    } else {
        unparseObject(child, level, flags);
    }
}

void
impl::Writer::writeTrailer(
    trailer_e which, int size, bool xref_stream, qpdf_offset_t prev, int linearization_pass)
{
    auto trailer = trimmed_trailer();
    if (xref_stream) {
        cur_data_key.clear();
    } else {
        write("trailer <<");
    }
    write_qdf("\n");
    if (which == t_lin_second) {
        write(" /Size ").write(size);
    } else {
        for (auto const& [key, value]: trailer) {
            if (value.null()) {
                continue;
            }
            write_qdf("  ").write_no_qdf(" ").write_name(key).write(" ");
            if (key == "/Size") {
                write(size);
                if (which == t_lin_first) {
                    write(" /Prev ");
                    qpdf_offset_t pos = pipeline->getCount();
                    write(prev).write(QIntC::to_size(pos - pipeline->getCount() + 21), ' ');
                }
            } else {
                unparseChild(value, 1, 0);
            }
            write_qdf("\n");
        }
    }

    // Write ID
    write_qdf(" ").write(" /ID [");
    if (linearization_pass == 1) {
        std::string original_id1 = getOriginalID1();
        if (original_id1.empty()) {
            write("<00000000000000000000000000000000>");
        } else {
            // Write a string of zeroes equal in length to the representation of the original ID.
            // While writing the original ID would have the same number of bytes, it would cause a
            // change to the deterministic ID generated by older versions of the software that
            // hard-coded the length of the ID to 16 bytes.
            size_t len = QPDF_String(original_id1).unparse(true).length() - 2;
            write("<").write(len, '0').write(">");
        }
        write("<00000000000000000000000000000000>");
    } else {
        if (linearization_pass == 0 && cfg.deterministic_id()) {
            computeDeterministicIDData();
        }
        generateID(encryption.get());
        write_string(id1, true).write_string(id2, true);
    }
    write("]");

    if (which != t_lin_second) {
        // Write reference to encryption dictionary
        if (encryption) {
            write(" /Encrypt ").write(encryption_dict_objid).write(" 0 R");
        }
    }

    write_qdf("\n>>").write_no_qdf(" >>");
}

bool
impl::Writer::will_filter_stream(QPDFObjectHandle stream)
{
    std::string s;
    [[maybe_unused]] auto [filter, ignore1, ignore2] = will_filter_stream(stream, &s);
    return filter;
}

std::tuple<const bool, const bool, const bool>
impl::Writer::will_filter_stream(QPDFObjectHandle stream, std::string* stream_data)
{
    const bool is_root_metadata = stream.isRootMetadata();
    bool filter = false;
    auto decode_level = cfg.decode_level();
    int encode_flags = 0;
    Dictionary stream_dict = stream.getDict();

    if (stream.getFilterOnWrite()) {
        filter = stream.isDataModified() || cfg.compress_streams() || decode_level != qpdf_dl_none;
        if (cfg.compress_streams()) {
            // Don't filter if the stream is already compressed with FlateDecode. This way we don't
            // make it worse if the original file used a better Flate algorithm, and we don't spend
            // time and CPU cycles uncompressing and recompressing stuff. This can be overridden
            // with setRecompressFlate(true).
            Name Filter = stream_dict["/Filter"];
            if (Filter && !cfg.recompress_flate() && !stream.isDataModified() &&
                (Filter == "/FlateDecode" || Filter == "/Fl")) {
                filter = false;
            }
        }
        if (is_root_metadata && (!encryption || !encryption->getEncryptMetadata())) {
            filter = true;
            decode_level = qpdf_dl_all;
        } else if (cfg.normalize_content() && normalized_streams.contains(stream)) {
            encode_flags = qpdf_ef_normalize;
            filter = true;
        } else if (filter && cfg.compress_streams()) {
            encode_flags = qpdf_ef_compress;
        }
    }

    // Disable compression for empty streams to improve compatibility
    if (Integer(stream_dict["/Length"]) == 0) {
        filter = true;
        encode_flags = 0;
    }

    for (bool first_attempt: {true, false}) {
        auto pp_stream_data =
            stream_data ? pipeline_stack.activate(*stream_data) : pipeline_stack.activate(true);

        try {
            if (stream.pipeStreamData(
                    pipeline,
                    filter ? encode_flags : 0,
                    filter ? decode_level : qpdf_dl_none,
                    false,
                    first_attempt)) {
                return {true, encode_flags & qpdf_ef_compress, is_root_metadata};
            }
            if (!filter) {
                break;
            }
        } catch (std::runtime_error& e) {
            if (!(filter && first_attempt)) {
                throw std::runtime_error(
                    "error while getting stream data for " + stream.unparse() + ": " + e.what());
            }
            stream.warn("error while getting stream data: "s + e.what());
            stream.warn("qpdf will attempt to write the damaged stream unchanged");
        }
        // Try again
        filter = false;
        stream.setFilterOnWrite(false);
        if (stream_data) {
            stream_data->clear();
        }
    }
    return {false, false, is_root_metadata};
}

void
impl::Writer::unparseObject(
    QPDFObjectHandle object, size_t level, int flags, size_t stream_length, bool compress)
{
    QPDFObjGen old_og = object.getObjGen();
    int child_flags = flags & ~f_stream;
    // For non-qdf, "indent" and "indent_large" are a single space between tokens. For qdf, they
    // include the preceding newline.
    std::string indent_large = " ";
    if (cfg.qdf()) {
        indent_large.append(2 * (level + 1), ' ');
        indent_large[0] = '\n';
    }
    std::string_view indent{indent_large.data(), cfg.qdf() ? indent_large.size() - 2 : 1};

    if (auto const tc = object.getTypeCode(); tc == ::ot_array) {
        // Note: PDF spec 1.4 implementation note 121 states that Acrobat requires a space after the
        // [ in the /H key of the linearization parameter dictionary.  We'll do this unconditionally
        // for all arrays because it looks nicer and doesn't make the files that much bigger.
        write("[");
        for (auto const& item: object.as_array()) {
            write(indent_large);
            unparseChild(item, level + 1, child_flags);
        }
        write(indent).write("]");
    } else if (tc == ::ot_dictionary) {
        // Handle special cases for specific dictionaries.

        if (old_og == root_og) {
            // Extensions dictionaries.

            // We have one of several cases:
            //
            // * We need ADBE
            //    - We already have Extensions
            //       - If it has the right ADBE, preserve it
            //       - Otherwise, replace ADBE
            //    - We don't have Extensions: create one from scratch
            // * We don't want ADBE
            //    - We already have Extensions
            //       - If it only has ADBE, remove it
            //       - If it has other things, keep those and remove ADBE
            //    - We have no extensions: no action required
            //
            // Before writing, we guarantee that /Extensions, if present, is direct through the ADBE
            // dictionary, so we can modify in place.

            auto extensions = object.getKey("/Extensions");
            const bool has_extensions = extensions.isDictionary();
            const bool need_extensions_adbe = final_extension_level > 0;

            if (has_extensions || need_extensions_adbe) {
                // Make a shallow copy of this object so we can modify it safely without affecting
                // the original. This code has logic to skip certain keys in agreement with
                // prepareFileForWrite and with skip_stream_parameters so that replacing them
                // doesn't leave unreferenced objects in the output. We can use unsafeShallowCopy
                // here because all we are doing is removing or replacing top-level keys.
                object = object.unsafeShallowCopy();
                if (!has_extensions) {
                    extensions = QPDFObjectHandle();
                }

                const bool have_extensions_adbe = extensions && extensions.hasKey("/ADBE");
                const bool have_extensions_other =
                    extensions && extensions.getKeys().size() > (have_extensions_adbe ? 1u : 0u);

                if (need_extensions_adbe) {
                    if (!(have_extensions_other || have_extensions_adbe)) {
                        // We need Extensions and don't have it.  Create it here.
                        QTC::TC("qpdf", "QPDFWriter create Extensions", cfg.qdf() ? 0 : 1);
                        extensions = object.replaceKeyAndGetNew(
                            "/Extensions", QPDFObjectHandle::newDictionary());
                    }
                } else if (!have_extensions_other) {
                    // We have Extensions dictionary and don't want one.
                    if (have_extensions_adbe) {
                        QTC::TC("qpdf", "QPDFWriter remove existing Extensions");
                        object.removeKey("/Extensions");
                        extensions = QPDFObjectHandle(); // uninitialized
                    }
                }

                if (extensions) {
                    QTC::TC("qpdf", "QPDFWriter preserve Extensions");
                    QPDFObjectHandle adbe = extensions.getKey("/ADBE");
                    if (adbe.isDictionary() &&
                        adbe.getKey("/BaseVersion").isNameAndEquals("/" + final_pdf_version) &&
                        adbe.getKey("/ExtensionLevel").isInteger() &&
                        (adbe.getKey("/ExtensionLevel").getIntValue() == final_extension_level)) {
                    } else {
                        if (need_extensions_adbe) {
                            extensions.replaceKey(
                                "/ADBE",
                                QPDFObjectHandle::parse(
                                    "<< /BaseVersion /" + final_pdf_version + " /ExtensionLevel " +
                                    std::to_string(final_extension_level) + " >>"));
                        } else {
                            extensions.removeKey("/ADBE");
                        }
                    }
                }
            }
        }

        // Stream dictionaries.

        if (flags & f_stream) {
            // Suppress /Length since we will write it manually

            // Make a shallow copy of this object so we can modify it safely without affecting the
            // original. This code has logic to skip certain keys in agreement with
            // prepareFileForWrite and with skip_stream_parameters so that replacing them doesn't
            // leave unreferenced objects in the output. We can use unsafeShallowCopy here because
            // all we are doing is removing or replacing top-level keys.
            object = object.unsafeShallowCopy();

            object.removeKey("/Length");

            // If /DecodeParms is an empty list, remove it.
            if (object.getKey("/DecodeParms").empty()) {
                object.removeKey("/DecodeParms");
            }

            if (flags & f_filtered) {
                // We will supply our own filter and decode parameters.
                object.removeKey("/Filter");
                object.removeKey("/DecodeParms");
            } else {
                // Make sure, no matter what else we have, that we don't have /Crypt in the output
                // filters.
                QPDFObjectHandle filter = object.getKey("/Filter");
                QPDFObjectHandle decode_parms = object.getKey("/DecodeParms");
                if (filter.isOrHasName("/Crypt")) {
                    if (filter.isName()) {
                        object.removeKey("/Filter");
                        object.removeKey("/DecodeParms");
                    } else {
                        int idx = 0;
                        for (auto const& item: filter.as_array()) {
                            if (item.isNameAndEquals("/Crypt")) {
                                // If filter is an array, then the code in QPDF_Stream has already
                                // verified that DecodeParms and Filters are arrays of the same
                                // length, but if they weren't for some reason, eraseItem does type
                                // and bounds checking. Fuzzing tells us that this can actually
                                // happen.
                                filter.eraseItem(idx);
                                decode_parms.eraseItem(idx);
                                break;
                            }
                            ++idx;
                        }
                    }
                }
            }
        }

        write("<<");

        for (auto const& [key, value]: object.as_dictionary()) {
            if (!value.null()) {
                write(indent_large).write_name(key).write(" ");
                if (key == "/Contents" && object.isDictionaryOfType("/Sig") &&
                    object.hasKey("/ByteRange")) {
                    QTC::TC("qpdf", "QPDFWriter no encryption sig contents");
                    unparseChild(value, level + 1, child_flags | f_hex_string | f_no_encryption);
                } else {
                    unparseChild(value, level + 1, child_flags);
                }
            }
        }

        if (flags & f_stream) {
            write(indent_large).write("/Length ");

            if (cfg.direct_stream_lengths()) {
                write(stream_length);
            } else {
                write(cur_stream_length_id).write(" 0 R");
            }
            if (compress && (flags & f_filtered)) {
                write(indent_large).write("/Filter /FlateDecode");
            }
        }

        write(indent).write(">>");
    } else if (tc == ::ot_stream) {
        // Write stream data to a buffer.
        if (!cfg.direct_stream_lengths()) {
            cur_stream_length_id = obj[old_og].renumber + 1;
        }

        flags |= f_stream;
        std::string stream_data;
        auto [filter, compress_stream, is_root_metadata] = will_filter_stream(object, &stream_data);
        if (filter) {
            flags |= f_filtered;
        }
        QPDFObjectHandle stream_dict = object.getDict();

        cur_stream_length = stream_data.size();
        if (is_root_metadata && encryption && !encryption->getEncryptMetadata()) {
            // Don't encrypt stream data for the metadata stream
            cur_data_key.clear();
        }
        adjustAESStreamLength(cur_stream_length);
        unparseObject(stream_dict, 0, flags, cur_stream_length, compress_stream);
        char last_char = stream_data.empty() ? '\0' : stream_data.back();
        write("\nstream\n").write_encrypted(stream_data);
        added_newline = cfg.newline_before_endstream() || (cfg.qdf() && last_char != '\n');
        write(added_newline ? "\nendstream" : "endstream");
    } else if (tc == ::ot_string) {
        std::string val;
        if (encryption && !(flags & f_in_ostream) && !(flags & f_no_encryption) &&
            !cur_data_key.empty()) {
            val = object.getStringValue();
            if (cfg.encrypt_use_aes()) {
                Pl_Buffer bufpl("encrypted string");
                Pl_AES_PDF pl("aes encrypt string", &bufpl, true, cur_data_key);
                pl.writeString(val);
                pl.finish();
                val = QPDF_String(bufpl.getString()).unparse(true);
            } else {
                auto tmp_ph = QUtil::make_unique_cstr(val);
                char* tmp = tmp_ph.get();
                size_t vlen = val.length();
                RC4 rc4(
                    QUtil::unsigned_char_pointer(cur_data_key),
                    QIntC::to_int(cur_data_key.length()));
                auto data = QUtil::unsigned_char_pointer(tmp);
                rc4.process(data, vlen, data);
                val = QPDF_String(std::string(tmp, vlen)).unparse();
            }
        } else if (flags & f_hex_string) {
            val = QPDF_String(object.getStringValue()).unparse(true);
        } else {
            val = object.unparseResolved();
        }
        write(val);
    } else {
        write(object.unparseResolved());
    }
}

void
impl::Writer::writeObjectStreamOffsets(std::vector<qpdf_offset_t>& offsets, int first_obj)
{
    qpdf_assert_debug(first_obj > 0);
    bool is_first = true;
    auto id = std::to_string(first_obj) + ' ';
    for (auto& offset: offsets) {
        if (is_first) {
            is_first = false;
        } else {
            write_qdf("\n").write_no_qdf(" ");
        }
        write(id);
        util::increment(id, 1);
        write(offset);
    }
    write("\n");
}

void
impl::Writer::writeObjectStream(QPDFObjectHandle object)
{
    // Note: object might be null if this is a place-holder for an object stream that we are
    // generating from scratch.

    QPDFObjGen old_og = object.getObjGen();
    qpdf_assert_debug(old_og.getGen() == 0);
    int old_id = old_og.getObj();
    int new_stream_id = obj[old_og].renumber;

    std::vector<qpdf_offset_t> offsets;
    qpdf_offset_t first = 0;

    // Generate stream itself.  We have to do this in two passes so we can calculate offsets in the
    // first pass.
    std::string stream_buffer_pass1;
    std::string stream_buffer_pass2;
    int first_obj = -1;
    const bool compressed = cfg.compress_streams() && !cfg.qdf();
    {
        // Pass 1
        auto pp_ostream_pass1 = pipeline_stack.activate(stream_buffer_pass1);

        int count = -1;
        for (auto const& og: object_stream_to_objects[old_id]) {
            ++count;
            int new_o = obj[og].renumber;
            if (first_obj == -1) {
                first_obj = new_o;
            }
            if (cfg.qdf()) {
                write("%% Object stream: object ").write(new_o).write(", index ").write(count);
                if (!cfg.no_original_object_ids()) {
                    write("; original object ID: ").write(og.getObj());
                    // For compatibility, only write the generation if non-zero.  While object
                    // streams only allow objects with generation 0, if we are generating object
                    // streams, the old object could have a non-zero generation.
                    if (og.getGen() != 0) {
                        write(" ").write(og.getGen());
                    }
                }
                write("\n");
            }

            offsets.push_back(pipeline->getCount());
            // To avoid double-counting objects being written in object streams for progress
            // reporting, decrement in pass 1.
            indicateProgress(true, false);

            QPDFObjectHandle obj_to_write = qpdf.getObject(og);
            if (obj_to_write.isStream()) {
                // This condition occurred in a fuzz input. Ideally we should block it at parse
                // time, but it's not clear to me how to construct a case for this.
                obj_to_write.warn("stream found inside object stream; treating as null");
                obj_to_write = QPDFObjectHandle::newNull();
            }
            writeObject(obj_to_write, count);

            new_obj[new_o].xref = QPDFXRefEntry(new_stream_id, count);
        }
    }
    {
        // Adjust offsets to skip over comment before first object
        first = offsets.at(0);
        for (auto& iter: offsets) {
            iter -= first;
        }

        // Take one pass at writing pairs of numbers so we can get their size information
        {
            auto pp_discard = pipeline_stack.activate(true);
            writeObjectStreamOffsets(offsets, first_obj);
            first += pipeline->getCount();
        }

        // Set up a stream to write the stream data into a buffer.
        auto pp_ostream = pipeline_stack.activate(stream_buffer_pass2);

        writeObjectStreamOffsets(offsets, first_obj);
        write(stream_buffer_pass1);
        stream_buffer_pass1.clear();
        stream_buffer_pass1.shrink_to_fit();
        if (compressed) {
            stream_buffer_pass2 = pl::pipe<Pl_Flate>(stream_buffer_pass2, Pl_Flate::a_deflate);
        }
    }

    // Write the object
    openObject(new_stream_id);
    setDataKey(new_stream_id);
    write("<<").write_qdf("\n ").write(" /Type /ObjStm").write_qdf("\n ");
    size_t length = stream_buffer_pass2.size();
    adjustAESStreamLength(length);
    write(" /Length ").write(length).write_qdf("\n ");
    if (compressed) {
        write(" /Filter /FlateDecode");
    }
    write(" /N ").write(offsets.size()).write_qdf("\n ").write(" /First ").write(first);
    if (!object.null()) {
        // If the original object has an /Extends key, preserve it.
        QPDFObjectHandle dict = object.getDict();
        QPDFObjectHandle extends = dict.getKey("/Extends");
        if (extends.isIndirect()) {
            write_qdf("\n ").write(" /Extends ");
            unparseChild(extends, 1, f_in_ostream);
        }
    }
    write_qdf("\n").write_no_qdf(" ").write(">>\nstream\n").write_encrypted(stream_buffer_pass2);
    write(cfg.newline_before_endstream() ? "\nendstream" : "endstream");
    if (encryption) {
        cur_data_key.clear();
    }
    closeObject(new_stream_id);
}

void
impl::Writer::writeObject(QPDFObjectHandle object, int object_stream_index)
{
    QPDFObjGen old_og = object.getObjGen();

    if (object_stream_index == -1 && old_og.getGen() == 0 &&
        object_stream_to_objects.contains(old_og.getObj())) {
        writeObjectStream(object);
        return;
    }

    indicateProgress(false, false);
    auto new_id = obj[old_og].renumber;
    if (cfg.qdf()) {
        if (page_object_to_seq.contains(old_og)) {
            write("%% Page ").write(page_object_to_seq[old_og]).write("\n");
        }
        if (contents_to_page_seq.contains(old_og)) {
            write("%% Contents for page ").write(contents_to_page_seq[old_og]).write("\n");
        }
    }
    if (object_stream_index == -1) {
        if (cfg.qdf() && !cfg.no_original_object_ids()) {
            write("%% Original object ID: ").write(object.getObjGen().unparse(' ')).write("\n");
        }
        openObject(new_id);
        setDataKey(new_id);
        unparseObject(object, 0, 0);
        cur_data_key.clear();
        closeObject(new_id);
    } else {
        unparseObject(object, 0, f_in_ostream);
        write("\n");
    }

    if (!cfg.direct_stream_lengths() && object.isStream()) {
        if (cfg.qdf()) {
            if (added_newline) {
                write("%QDF: ignore_newline\n");
            }
        }
        openObject(new_id + 1);
        write(cur_stream_length);
        closeObject(new_id + 1);
    }
}

std::string
impl::Writer::getOriginalID1()
{
    QPDFObjectHandle trailer = qpdf.getTrailer();
    if (trailer.hasKey("/ID")) {
        return trailer.getKey("/ID").getArrayItem(0).getStringValue();
    } else {
        return "";
    }
}

void
impl::Writer::generateID(bool encrypted)
{
    // Generate the ID lazily so that we can handle the user's preference to use static or
    // deterministic ID generation.

    if (!id2.empty()) {
        return;
    }

    QPDFObjectHandle trailer = qpdf.getTrailer();

    std::string result;

    if (cfg.static_id()) {
        // For test suite use only...
        static unsigned char tmp[] = {
            0x31,
            0x41,
            0x59,
            0x26,
            0x53,
            0x58,
            0x97,
            0x93,
            0x23,
            0x84,
            0x62,
            0x64,
            0x33,
            0x83,
            0x27,
            0x95,
            0x00};
        result = reinterpret_cast<char*>(tmp);
    } else {
        // The PDF specification has guidelines for creating IDs, but it states clearly that the
        // only thing that's really important is that it is very likely to be unique.  We can't
        // really follow the guidelines in the spec exactly because we haven't written the file yet.
        // This scheme should be fine though.  The deterministic ID case uses a digest of a
        // sufficient portion of the file's contents such no two non-matching files would match in
        // the subsets used for this computation.  Note that we explicitly omit the filename from
        // the digest calculation for deterministic ID so that the same file converted with qpdf, in
        // that case, would have the same ID regardless of the output file's name.

        std::string seed;
        if (cfg.deterministic_id()) {
            if (encrypted) {
                throw std::runtime_error(
                    "QPDFWriter: unable to generated a deterministic ID because the file to be "
                    "written is encrypted (even though the file may not require a password)");
            }
            if (deterministic_id_data.empty()) {
                throw std::logic_error(
                    "INTERNAL ERROR: QPDFWriter::generateID has no data for deterministic ID");
            }
            seed += deterministic_id_data;
        } else {
            seed += std::to_string(QUtil::get_current_time());
            seed += filename;
            seed += " ";
        }
        seed += " QPDF ";
        if (trailer.hasKey("/Info")) {
            for (auto const& item: trailer.getKey("/Info").as_dictionary()) {
                if (item.second.isString()) {
                    seed += " ";
                    seed += item.second.getStringValue();
                }
            }
        }

        MD5 md5;
        md5.encodeString(seed.c_str());
        MD5::Digest digest;
        md5.digest(digest);
        result = std::string(reinterpret_cast<char*>(digest), sizeof(MD5::Digest));
    }

    // If /ID already exists, follow the spec: use the original first word and generate a new second
    // word.  Otherwise, we'll use the generated ID for both.

    id2 = result;
    // Note: keep /ID from old file even if --static-id was given.
    id1 = getOriginalID1();
    if (id1.empty()) {
        id1 = id2;
    }
}

void
impl::Writer::initializeSpecialStreams()
{
    // Mark all page content streams in case we are filtering or normalizing.
    int num = 0;
    for (auto& page: pages) {
        page_object_to_seq[page.getObjGen()] = ++num;
        QPDFObjectHandle contents = page.getKey("/Contents");
        std::vector<QPDFObjGen> contents_objects;
        if (contents.isArray()) {
            int n = static_cast<int>(contents.size());
            for (int i = 0; i < n; ++i) {
                contents_objects.push_back(contents.getArrayItem(i).getObjGen());
            }
        } else if (contents.isStream()) {
            contents_objects.push_back(contents.getObjGen());
        }

        for (auto const& c: contents_objects) {
            contents_to_page_seq[c] = num;
            normalized_streams.insert(c);
        }
    }
}

void
impl::Writer::preserveObjectStreams()
{
    auto const& xref = objects.xref_table();
    // Our object_to_object_stream map has to map ObjGen -> ObjGen since we may be generating object
    // streams out of old objects that have generation numbers greater than zero. However in an
    // existing PDF, all object stream objects and all objects in them must have generation 0
    // because the PDF spec does not provide any way to do otherwise. This code filters out objects
    // that are not allowed to be in object streams. In addition to removing objects that were
    // erroneously included in object streams in the source PDF, it also prevents unreferenced
    // objects from being included.
    auto end = xref.cend();
    obj.streams_empty = true;
    if (cfg.preserve_unreferenced()) {
        for (auto iter = xref.cbegin(); iter != end; ++iter) {
            if (iter->second.getType() == 2) {
                // Pdf contains object streams.
                obj.streams_empty = false;
                obj[iter->first].object_stream = iter->second.getObjStreamNumber();
            }
        }
    } else {
        // Start by scanning for first compressed object in case we don't have any object streams to
        // process.
        for (auto iter = xref.cbegin(); iter != end; ++iter) {
            if (iter->second.getType() == 2) {
                // Pdf contains object streams.
                obj.streams_empty = false;
                auto eligible = objects.compressible_set();
                // The object pointed to by iter may be a previous generation, in which case it is
                // removed by compressible_set. We need to restart the loop (while the object
                // table may contain multiple generations of an object).
                for (iter = xref.cbegin(); iter != end; ++iter) {
                    if (iter->second.getType() == 2) {
                        auto id = static_cast<size_t>(iter->first.getObj());
                        if (id < eligible.size() && eligible[id]) {
                            obj[iter->first].object_stream = iter->second.getObjStreamNumber();
                        } else {
                            QTC::TC("qpdf", "QPDFWriter exclude from object stream");
                        }
                    }
                }
                return;
            }
        }
    }
}

void
impl::Writer::generateObjectStreams()
{
    // Basic strategy: make a list of objects that can go into an object stream.  Then figure out
    // how many object streams are needed so that we can distribute objects approximately evenly
    // without having any object stream exceed 100 members.  We don't have to worry about linearized
    // files here -- if the file is linearized, we take care of excluding things that aren't allowed
    // here later.

    // This code doesn't do anything with /Extends.

    auto eligible = objects.compressible_vector();
    size_t n_object_streams = (eligible.size() + 99U) / 100U;

    initializeTables(2U * n_object_streams);
    if (n_object_streams == 0) {
        obj.streams_empty = true;
        return;
    }
    size_t n_per = eligible.size() / n_object_streams;
    if (n_per * n_object_streams < eligible.size()) {
        ++n_per;
    }
    unsigned int n = 0;
    int cur_ostream = qpdf.newIndirectNull().getObjectID();
    for (auto const& item: eligible) {
        if (n == n_per) {
            n = 0;
            // Construct a new null object as the "original" object stream.  The rest of the code
            // knows that this means we're creating the object stream from scratch.
            cur_ostream = qpdf.newIndirectNull().getObjectID();
        }
        auto& o = obj[item];
        o.object_stream = cur_ostream;
        o.gen = item.getGen();
        ++n;
    }
}

Dictionary
impl::Writer::trimmed_trailer()
{
    // Remove keys from the trailer that necessarily have to be replaced when writing the file.

    Dictionary trailer = qpdf.getTrailer().unsafeShallowCopy();

    // Remove encryption keys
    trailer.erase("/ID");
    trailer.erase("/Encrypt");

    // Remove modification information
    trailer.erase("/Prev");

    // Remove all trailer keys that potentially come from a cross-reference stream
    trailer.erase("/Index");
    trailer.erase("/W");
    trailer.erase("/Length");
    trailer.erase("/Filter");
    trailer.erase("/DecodeParms");
    trailer.erase("/Type");
    trailer.erase("/XRefStm");

    return trailer;
}

// Make document extension level information direct as required by the spec.
void
impl::Writer::prepareFileForWrite()
{
    qpdf.fixDanglingReferences();
    auto root = qpdf.getRoot();
    auto oh = root.getKey("/Extensions");
    if (oh.isDictionary()) {
        const bool extensions_indirect = oh.isIndirect();
        if (extensions_indirect) {
            QTC::TC("qpdf", "QPDFWriter make Extensions direct");
            oh = root.replaceKeyAndGetNew("/Extensions", oh.shallowCopy());
        }
        if (oh.hasKey("/ADBE")) {
            auto adbe = oh.getKey("/ADBE");
            if (adbe.isIndirect()) {
                QTC::TC("qpdf", "QPDFWriter make ADBE direct", extensions_indirect ? 0 : 1);
                adbe.makeDirect();
                oh.replaceKey("/ADBE", adbe);
            }
        }
    }
}

void
impl::Writer::initializeTables(size_t extra)
{
    auto size = objects.table_size() + 100u + extra;
    obj.resize(size);
    new_obj.resize(size);
}

void
impl::Writer::doWriteSetup()
{
    if (did_write_setup) {
        return;
    }
    did_write_setup = true;

    // Do preliminary setup

    if (cfg.linearize()) {
        cfg.qdf(false);
    }

    if (cfg.pclm()) {
        encryption = nullptr;
    }

    if (encryption) {
        // Encryption has been explicitly set
        cfg.preserve_encryption(false);
    } else if (cfg.normalize_content() || cfg.pclm()) {
        // Encryption makes looking at contents pretty useless.  If the user explicitly encrypted
        // though, we still obey that.
        cfg.preserve_encryption(false);
    }

    if (cfg.preserve_encryption()) {
        copyEncryptionParameters(qpdf);
    }

    if (!cfg.forced_pdf_version().empty()) {
        int major = 0;
        int minor = 0;
        parseVersion(cfg.forced_pdf_version(), major, minor);
        disableIncompatibleEncryption(major, minor, cfg.forced_extension_level());
        if (compareVersions(major, minor, 1, 5) < 0) {
            cfg.object_streams(qpdf_o_disable);
        }
    }

    if (cfg.qdf() || cfg.normalize_content()) {
        initializeSpecialStreams();
    }

    switch (cfg.object_streams()) {
    case qpdf_o_disable:
        initializeTables();
        obj.streams_empty = true;
        break;

    case qpdf_o_preserve:
        initializeTables();
        preserveObjectStreams();
        break;

    case qpdf_o_generate:
        generateObjectStreams();
        break;
    }

    if (!obj.streams_empty) {
        if (cfg.linearize()) {
            // Page dictionaries are not allowed to be compressed objects.
            for (auto& page: pages) {
                if (obj[page].object_stream > 0) {
                    obj[page].object_stream = 0;
                }
            }
        }

        if (cfg.linearize() || encryption) {
            // The document catalog is not allowed to be compressed in cfg.linearized_ files either.
            // It also appears that Adobe Reader 8.0.0 has a bug that prevents it from being able to
            // handle encrypted files with compressed document catalogs, so we disable them in that
            // case as well.
            if (obj[root_og].object_stream > 0) {
                obj[root_og].object_stream = 0;
            }
        }

        // Generate reverse mapping from object stream to objects
        obj.forEach([this](auto id, auto const& item) -> void {
            if (item.object_stream > 0) {
                auto& vec = object_stream_to_objects[item.object_stream];
                vec.emplace_back(id, item.gen);
                if (max_ostream_index < vec.size()) {
                    ++max_ostream_index;
                }
            }
        });
        --max_ostream_index;

        if (object_stream_to_objects.empty()) {
            obj.streams_empty = true;
        } else {
            setMinimumPDFVersion("1.5");
        }
    }

    setMinimumPDFVersion(qpdf.getPDFVersion(), qpdf.getExtensionLevel());
    final_pdf_version = min_pdf_version;
    final_extension_level = min_extension_level;
    if (!cfg.forced_pdf_version().empty()) {
        final_pdf_version = cfg.forced_pdf_version();
        final_extension_level = cfg.forced_extension_level();
    }
}

void
QPDFWriter::write()
{
    m->write();
}

void
impl::Writer::write()
{
    doWriteSetup();

    // Set up progress reporting. For linearized files, we write two passes. events_expected is an
    // approximation, but it's good enough for progress reporting, which is mostly a guess anyway.
    events_expected = QIntC::to_int(qpdf.getObjectCount() * (cfg.linearize() ? 2 : 1));

    prepareFileForWrite();

    if (cfg.linearize()) {
        writeLinearized();
    } else {
        writeStandard();
    }

    pipeline->finish();
    if (close_file) {
        fclose(file);
    }
    file = nullptr;
    if (buffer_pipeline) {
        output_buffer = buffer_pipeline->getBuffer();
        buffer_pipeline = nullptr;
    }
    indicateProgress(false, true);
}

QPDFObjGen
QPDFWriter::getRenumberedObjGen(QPDFObjGen og)
{
    return {m->obj[og].renumber, 0};
}

std::map<QPDFObjGen, QPDFXRefEntry>
QPDFWriter::getWrittenXRefTable()
{
    return m->getWrittenXRefTable();
}

std::map<QPDFObjGen, QPDFXRefEntry>
impl::Writer::getWrittenXRefTable()
{
    std::map<QPDFObjGen, QPDFXRefEntry> result;

    auto it = result.begin();
    new_obj.forEach([&it, &result](auto id, auto const& item) -> void {
        if (item.xref.getType() != 0) {
            it = result.emplace_hint(it, QPDFObjGen(id, 0), item.xref);
        }
    });
    return result;
}

void
impl::Writer::enqueuePart(std::vector<QPDFObjectHandle>& part)
{
    for (auto const& oh: part) {
        enqueue(oh);
    }
}

void
impl::Writer::writeEncryptionDictionary()
{
    encryption_dict_objid = openObject(encryption_dict_objid);
    auto& enc = *encryption;
    auto const V = enc.getV();

    write("<<");
    if (V >= 4) {
        write(" /CF << /StdCF << /AuthEvent /DocOpen /CFM ");
        write(cfg.encrypt_use_aes() ? (V < 5 ? "/AESV2" : "/AESV3") : "/V2");
        // The PDF spec says the /Length key is optional, but the PDF previewer on some versions of
        // MacOS won't open encrypted files without it.
        write(V < 5 ? " /Length 16 >> >>" : " /Length 32 >> >>");
        if (!encryption->getEncryptMetadata()) {
            write(" /EncryptMetadata false");
        }
    }
    write(" /Filter /Standard /Length ").write(enc.getLengthBytes() * 8);
    write(" /O ").write_string(enc.getO(), true);
    if (V >= 4) {
        write(" /OE ").write_string(enc.getOE(), true);
    }
    write(" /P ").write(enc.getP());
    if (V >= 5) {
        write(" /Perms ").write_string(enc.getPerms(), true);
    }
    write(" /R ").write(enc.getR());

    if (V >= 4) {
        write(" /StmF /StdCF /StrF /StdCF");
    }
    write(" /U ").write_string(enc.getU(), true);
    if (V >= 4) {
        write(" /UE ").write_string(enc.getUE(), true);
    }
    write(" /V ").write(enc.getV()).write(" >>");
    closeObject(encryption_dict_objid);
}

std::string
QPDFWriter::getFinalVersion()
{
    m->doWriteSetup();
    return m->final_pdf_version;
}

void
impl::Writer::writeHeader()
{
    write("%PDF-").write(final_pdf_version);
    if (cfg.pclm()) {
        // PCLm version
        write("\n%PCLm 1.0\n");
    } else {
        // This string of binary characters would not be valid UTF-8, so it really should be treated
        // as binary.
        write("\n%\xbf\xf7\xa2\xfe\n");
    }
    write_qdf("%QDF-1.0\n\n");

    // Note: do not write extra header text here.  Linearized PDFs must include the entire
    // linearization parameter dictionary within the first 1024 characters of the PDF file, so for
    // linearized files, we have to write extra header text after the linearization parameter
    // dictionary.
}

void
impl::Writer::writeHintStream(int hint_id)
{
    std::string hint_buffer;
    int S = 0;
    int O = 0;
    bool compressed = cfg.compress_streams();
    lin.generateHintStream(new_obj, obj, hint_buffer, S, O, compressed);

    openObject(hint_id);
    setDataKey(hint_id);

    size_t hlen = hint_buffer.size();

    write("<< ");
    if (compressed) {
        write("/Filter /FlateDecode ");
    }
    write("/S ").write(S);
    if (O) {
        write(" /O ").write(O);
    }
    adjustAESStreamLength(hlen);
    write(" /Length ").write(hlen);
    write(" >>\nstream\n").write_encrypted(hint_buffer);

    if (encryption) {
        QTC::TC("qpdf", "QPDFWriter encrypted hint stream");
    }

    write(hint_buffer.empty() || hint_buffer.back() != '\n' ? "\nendstream" : "endstream");
    closeObject(hint_id);
}

qpdf_offset_t
impl::Writer::writeXRefTable(trailer_e which, int first, int last, int size)
{
    // There are too many extra arguments to replace overloaded function with defaults in the header
    // file...too much risk of leaving something off.
    return writeXRefTable(which, first, last, size, 0, false, 0, 0, 0, 0);
}

qpdf_offset_t
impl::Writer::writeXRefTable(
    trailer_e which,
    int first,
    int last,
    int size,
    qpdf_offset_t prev,
    bool suppress_offsets,
    int hint_id,
    qpdf_offset_t hint_offset,
    qpdf_offset_t hint_length,
    int linearization_pass)
{
    write("xref\n").write(first).write(" ").write(last - first + 1);
    qpdf_offset_t space_before_zero = pipeline->getCount();
    write("\n");
    if (first == 0) {
        write("0000000000 65535 f \n");
        ++first;
    }
    for (int i = first; i <= last; ++i) {
        qpdf_offset_t offset = 0;
        if (!suppress_offsets) {
            offset = new_obj[i].xref.getOffset();
            if ((hint_id != 0) && (i != hint_id) && (offset >= hint_offset)) {
                offset += hint_length;
            }
        }
        write(QUtil::int_to_string(offset, 10)).write(" 00000 n \n");
    }
    writeTrailer(which, size, false, prev, linearization_pass);
    write("\n");
    return space_before_zero;
}

qpdf_offset_t
impl::Writer::writeXRefStream(
    int objid, int max_id, qpdf_offset_t max_offset, trailer_e which, int first, int last, int size)
{
    // There are too many extra arguments to replace overloaded function with defaults in the header
    // file...too much risk of leaving something off.
    return writeXRefStream(
        objid, max_id, max_offset, which, first, last, size, 0, 0, 0, 0, false, 0);
}

qpdf_offset_t
impl::Writer::writeXRefStream(
    int xref_id,
    int max_id,
    qpdf_offset_t max_offset,
    trailer_e which,
    int first,
    int last,
    int size,
    qpdf_offset_t prev,
    int hint_id,
    qpdf_offset_t hint_offset,
    qpdf_offset_t hint_length,
    bool skip_compression,
    int linearization_pass)
{
    qpdf_offset_t xref_offset = pipeline->getCount();
    qpdf_offset_t space_before_zero = xref_offset - 1;

    // field 1 contains offsets and object stream identifiers
    unsigned int f1_size = std::max(bytesNeeded(max_offset + hint_length), bytesNeeded(max_id));

    // field 2 contains object stream indices
    unsigned int f2_size = bytesNeeded(QIntC::to_longlong(max_ostream_index));

    unsigned int esize = 1 + f1_size + f2_size;

    // Must store in xref table in advance of writing the actual data rather than waiting for
    // openObject to do it.
    new_obj[xref_id].xref = QPDFXRefEntry(pipeline->getCount());

    std::string xref_data;
    const bool compressed = cfg.compress_streams() && !cfg.qdf();
    {
        auto pp_xref = pipeline_stack.activate(xref_data);

        for (int i = first; i <= last; ++i) {
            QPDFXRefEntry& e = new_obj[i].xref;
            switch (e.getType()) {
            case 0:
                writeBinary(0, 1);
                writeBinary(0, f1_size);
                writeBinary(0, f2_size);
                break;

            case 1:
                {
                    qpdf_offset_t offset = e.getOffset();
                    if ((hint_id != 0) && (i != hint_id) && (offset >= hint_offset)) {
                        offset += hint_length;
                    }
                    writeBinary(1, 1);
                    writeBinary(QIntC::to_ulonglong(offset), f1_size);
                    writeBinary(0, f2_size);
                }
                break;

            case 2:
                writeBinary(2, 1);
                writeBinary(QIntC::to_ulonglong(e.getObjStreamNumber()), f1_size);
                writeBinary(QIntC::to_ulonglong(e.getObjStreamIndex()), f2_size);
                break;

            default:
                throw std::logic_error("invalid type writing xref stream");
                break;
            }
        }
    }

    if (compressed) {
        xref_data = pl::pipe<Pl_PNGFilter>(xref_data, Pl_PNGFilter::a_encode, esize);
        if (!skip_compression) {
            // Write the stream dictionary for compression but don't actually compress.  This
            // helps us with computation of padding for pass 1 of linearization.
            xref_data = pl::pipe<Pl_Flate>(xref_data, Pl_Flate::a_deflate);
        }
    }

    openObject(xref_id);
    write("<<").write_qdf("\n ").write(" /Type /XRef").write_qdf("\n ");
    write(" /Length ").write(xref_data.size());
    if (compressed) {
        write_qdf("\n ").write(" /Filter /FlateDecode").write_qdf("\n ");
        write(" /DecodeParms << /Columns ").write(esize).write(" /Predictor 12 >>");
    }
    write_qdf("\n ").write(" /W [ 1 ").write(f1_size).write(" ").write(f2_size).write(" ]");
    if (!(first == 0 && last == (size - 1))) {
        write(" /Index [ ").write(first).write(" ").write(last - first + 1).write(" ]");
    }
    writeTrailer(which, size, true, prev, linearization_pass);
    write("\nstream\n").write(xref_data).write("\nendstream");
    closeObject(xref_id);
    return space_before_zero;
}

size_t
impl::Writer::calculateXrefStreamPadding(qpdf_offset_t xref_bytes)
{
    // This routine is called right after a linearization first pass xref stream has been written
    // without compression.  Calculate the amount of padding that would be required in the worst
    // case, assuming the number of uncompressed bytes remains the same. The worst case for zlib is
    // that the output is larger than the input by 6 bytes plus 5 bytes per 16K, and then we'll add
    // 10 extra bytes for number length increases.

    return QIntC::to_size(16 + (5 * ((xref_bytes + 16383) / 16384)));
}

void
impl::Writer::writeLinearized()
{
    // Optimize file and enqueue objects in order

    std::map<int, int> stream_cache;

    auto skip_stream_parameters = [this, &stream_cache](QPDFObjectHandle& stream) {
        if (auto& result = stream_cache[stream.getObjectID()]) {
            return result;
        } else {
            return result = will_filter_stream(stream) ? 2 : 1;
        }
    };

    lin.optimize(obj, skip_stream_parameters);

    std::vector<QPDFObjectHandle> part4;
    std::vector<QPDFObjectHandle> part6;
    std::vector<QPDFObjectHandle> part7;
    std::vector<QPDFObjectHandle> part8;
    std::vector<QPDFObjectHandle> part9;
    lin.parts(obj, part4, part6, part7, part8, part9);

    // Object number sequence:
    //
    //  second half
    //    second half uncompressed objects
    //    second half xref stream, if any
    //    second half compressed objects
    //  first half
    //    linearization dictionary
    //    first half xref stream, if any
    //    part 4 uncompresesd objects
    //    encryption dictionary, if any
    //    hint stream
    //    part 6 uncompressed objects
    //    first half compressed objects
    //

    // Second half objects
    int second_half_uncompressed = QIntC::to_int(part7.size() + part8.size() + part9.size());
    int second_half_first_obj = 1;
    int after_second_half = 1 + second_half_uncompressed;
    next_objid = after_second_half;
    int second_half_xref = 0;
    bool need_xref_stream = !obj.streams_empty;
    if (need_xref_stream) {
        second_half_xref = next_objid++;
    }
    // Assign numbers to all compressed objects in the second half.
    std::vector<QPDFObjectHandle>* vecs2[] = {&part7, &part8, &part9};
    for (int i = 0; i < 3; ++i) {
        for (auto const& oh: *vecs2[i]) {
            assignCompressedObjectNumbers(oh.getObjGen());
        }
    }
    int second_half_end = next_objid - 1;
    int second_trailer_size = next_objid;

    // First half objects
    int first_half_start = next_objid;
    int lindict_id = next_objid++;
    int first_half_xref = 0;
    if (need_xref_stream) {
        first_half_xref = next_objid++;
    }
    int part4_first_obj = next_objid;
    next_objid += QIntC::to_int(part4.size());
    int after_part4 = next_objid;
    if (encryption) {
        encryption_dict_objid = next_objid++;
    }
    int hint_id = next_objid++;
    int part6_first_obj = next_objid;
    next_objid += QIntC::to_int(part6.size());
    int after_part6 = next_objid;
    // Assign numbers to all compressed objects in the first half
    std::vector<QPDFObjectHandle>* vecs1[] = {&part4, &part6};
    for (int i = 0; i < 2; ++i) {
        for (auto const& oh: *vecs1[i]) {
            assignCompressedObjectNumbers(oh.getObjGen());
        }
    }
    int first_half_end = next_objid - 1;
    int first_trailer_size = next_objid;

    int part4_end_marker = part4.back().getObjectID();
    int part6_end_marker = part6.back().getObjectID();
    qpdf_offset_t space_before_zero = 0;
    qpdf_offset_t file_size = 0;
    qpdf_offset_t part6_end_offset = 0;
    qpdf_offset_t first_half_max_obj_offset = 0;
    qpdf_offset_t second_xref_offset = 0;
    qpdf_offset_t first_xref_end = 0;
    qpdf_offset_t second_xref_end = 0;

    next_objid = part4_first_obj;
    enqueuePart(part4);
    if (next_objid != after_part4) {
        // This can happen with very botched files as in the fuzzer test. There are likely some
        // faulty assumptions in calculateLinearizationData
        throw std::runtime_error("error encountered after writing part 4 of linearized data");
    }
    next_objid = part6_first_obj;
    enqueuePart(part6);
    if (next_objid != after_part6) {
        throw std::runtime_error("error encountered after writing part 6 of linearized data");
    }
    next_objid = second_half_first_obj;
    enqueuePart(part7);
    enqueuePart(part8);
    enqueuePart(part9);
    if (next_objid != after_second_half) {
        throw std::runtime_error("error encountered after writing part 9 of cfg.linearized_ data");
    }

    qpdf_offset_t hint_length = 0;
    std::string hint_buffer;

    // Write file in two passes.  Part numbers refer to PDF spec 1.4.

    FILE* lin_pass1_file = nullptr;
    auto pp_pass1 = pipeline_stack.popper();
    auto pp_md5 = pipeline_stack.popper();
    for (int pass: {1, 2}) {
        if (pass == 1) {
            if (!cfg.linearize_pass1().empty()) {
                lin_pass1_file = QUtil::safe_fopen(cfg.linearize_pass1().data(), "wb");
                pipeline_stack.activate(
                    pp_pass1,
                    std::make_unique<Pl_StdioFile>("linearization pass1", lin_pass1_file));
            } else {
                pipeline_stack.activate(pp_pass1, true);
            }
            if (cfg.deterministic_id()) {
                pipeline_stack.activate_md5(pp_md5);
            }
        }

        // Part 1: header

        writeHeader();

        // Part 2: linearization parameter dictionary.  Save enough space to write real dictionary.
        // 200 characters is enough space if all numerical values in the parameter dictionary that
        // contain offsets are 20 digits long plus a few extra characters for safety.  The entire
        // linearization parameter dictionary must appear within the first 1024 characters of the
        // file.

        qpdf_offset_t pos = pipeline->getCount();
        openObject(lindict_id);
        write("<<");
        if (pass == 2) {
            write(" /Linearized 1 /L ").write(file_size + hint_length);
            // Implementation note 121 states that a space is mandatory after this open bracket.
            write(" /H [ ").write(new_obj[hint_id].xref.getOffset()).write(" ");
            write(hint_length);
            write(" ] /O ").write(obj[pages.all().at(0)].renumber);
            write(" /E ").write(part6_end_offset + hint_length);
            write(" /N ").write(pages.size());
            write(" /T ").write(space_before_zero + hint_length);
        }
        write(" >>");
        closeObject(lindict_id);
        static int const pad = 200;
        write(QIntC::to_size(pos - pipeline->getCount() + pad), ' ').write("\n");

        // If the user supplied any additional header text, write it here after the linearization
        // parameter dictionary.
        write(cfg.extra_header_text());

        // Part 3: first page cross reference table and trailer.

        qpdf_offset_t first_xref_offset = pipeline->getCount();
        qpdf_offset_t hint_offset = 0;
        if (pass == 2) {
            hint_offset = new_obj[hint_id].xref.getOffset();
        }
        if (need_xref_stream) {
            // Must pad here too.
            if (pass == 1) {
                // Set first_half_max_obj_offset to a value large enough to force four bytes to be
                // reserved for each file offset.  This would provide adequate space for the xref
                // stream as long as the last object in page 1 starts with in the first 4 GB of the
                // file, which is extremely likely.  In the second pass, we will know the actual
                // value for this, but it's okay if it's smaller.
                first_half_max_obj_offset = 1 << 25;
            }
            pos = pipeline->getCount();
            writeXRefStream(
                first_half_xref,
                first_half_end,
                first_half_max_obj_offset,
                t_lin_first,
                first_half_start,
                first_half_end,
                first_trailer_size,
                hint_length + second_xref_offset,
                hint_id,
                hint_offset,
                hint_length,
                (pass == 1),
                pass);
            qpdf_offset_t endpos = pipeline->getCount();
            if (pass == 1) {
                // Pad so we have enough room for the real xref stream.
                write(calculateXrefStreamPadding(endpos - pos), ' ');
                first_xref_end = pipeline->getCount();
            } else {
                // Pad so that the next object starts at the same place as in pass 1.
                write(QIntC::to_size(first_xref_end - endpos), ' ');

                if (pipeline->getCount() != first_xref_end) {
                    throw std::logic_error(
                        "insufficient padding for first pass xref stream; first_xref_end=" +
                        std::to_string(first_xref_end) + "; endpos=" + std::to_string(endpos));
                }
            }
            write("\n");
        } else {
            writeXRefTable(
                t_lin_first,
                first_half_start,
                first_half_end,
                first_trailer_size,
                hint_length + second_xref_offset,
                (pass == 1),
                hint_id,
                hint_offset,
                hint_length,
                pass);
            write("startxref\n0\n%%EOF\n");
        }

        // Parts 4 through 9

        for (auto const& cur_object: object_queue) {
            if (cur_object.getObjectID() == part6_end_marker) {
                first_half_max_obj_offset = pipeline->getCount();
            }
            writeObject(cur_object);
            if (cur_object.getObjectID() == part4_end_marker) {
                if (encryption) {
                    writeEncryptionDictionary();
                }
                if (pass == 1) {
                    new_obj[hint_id].xref = QPDFXRefEntry(pipeline->getCount());
                } else {
                    // Part 5: hint stream
                    write(hint_buffer);
                }
            }
            if (cur_object.getObjectID() == part6_end_marker) {
                part6_end_offset = pipeline->getCount();
            }
        }

        // Part 10: overflow hint stream -- not used

        // Part 11: main cross reference table and trailer

        second_xref_offset = pipeline->getCount();
        if (need_xref_stream) {
            pos = pipeline->getCount();
            space_before_zero = writeXRefStream(
                second_half_xref,
                second_half_end,
                second_xref_offset,
                t_lin_second,
                0,
                second_half_end,
                second_trailer_size,
                0,
                0,
                0,
                0,
                (pass == 1),
                pass);
            qpdf_offset_t endpos = pipeline->getCount();

            if (pass == 1) {
                // Pad so we have enough room for the real xref stream.  See comments for previous
                // xref stream on how we calculate the padding.
                write(calculateXrefStreamPadding(endpos - pos), ' ').write("\n");
                second_xref_end = pipeline->getCount();
            } else {
                // Make the file size the same.
                auto padding =
                    QIntC::to_size(second_xref_end + hint_length - 1 - pipeline->getCount());
                write(padding, ' ').write("\n");

                // If this assertion fails, maybe we didn't have enough padding above.
                if (pipeline->getCount() != second_xref_end + hint_length) {
                    throw std::logic_error(
                        "count mismatch after xref stream; possible insufficient padding?");
                }
            }
        } else {
            space_before_zero = writeXRefTable(
                t_lin_second, 0, second_half_end, second_trailer_size, 0, false, 0, 0, 0, pass);
        }
        write("startxref\n").write(first_xref_offset).write("\n%%EOF\n");

        if (pass == 1) {
            if (cfg.deterministic_id()) {
                QTC::TC("qpdf", "QPDFWriter linearized deterministic ID", need_xref_stream ? 0 : 1);
                computeDeterministicIDData();
                pp_md5.pop();
            }

            // Close first pass pipeline
            file_size = pipeline->getCount();
            pp_pass1.pop();

            // Save hint offset since it will be set to zero by calling openObject.
            qpdf_offset_t hint_offset1 = new_obj[hint_id].xref.getOffset();

            // Write hint stream to a buffer
            {
                auto pp_hint = pipeline_stack.activate(hint_buffer);
                writeHintStream(hint_id);
            }
            hint_length = QIntC::to_offset(hint_buffer.size());

            // Restore hint offset
            new_obj[hint_id].xref = QPDFXRefEntry(hint_offset1);
            if (lin_pass1_file) {
                // Write some debugging information
                fprintf(
                    lin_pass1_file, "%% hint_offset=%s\n", std::to_string(hint_offset1).c_str());
                fprintf(lin_pass1_file, "%% hint_length=%s\n", std::to_string(hint_length).c_str());
                fprintf(
                    lin_pass1_file,
                    "%% second_xref_offset=%s\n",
                    std::to_string(second_xref_offset).c_str());
                fprintf(
                    lin_pass1_file,
                    "%% second_xref_end=%s\n",
                    std::to_string(second_xref_end).c_str());
                fclose(lin_pass1_file);
                lin_pass1_file = nullptr;
            }
        }
    }
}

void
impl::Writer::enqueueObjectsStandard()
{
    if (cfg.preserve_unreferenced()) {
        for (auto const& oh: qpdf.getAllObjects()) {
            enqueue(oh);
        }
    }

    // Put root first on queue.
    auto trailer = trimmed_trailer();
    enqueue(trailer["/Root"]);

    // Next place any other objects referenced from the trailer dictionary into the queue, handling
    // direct objects recursively. Root is already there, so enqueuing it a second time is a no-op.
    for (auto& item: trailer) {
        if (!item.second.null()) {
            enqueue(item.second);
        }
    }
}

void
impl::Writer::enqueueObjectsPCLm()
{
    // Image transform stream content for page strip images. Each of this new stream has to come
    // after every page image strip written in the pclm file.
    std::string image_transform_content = "q /image Do Q\n";

    // enqueue all pages first
    for (auto& page: pages) {
        enqueue(page);
        enqueue(page["/Contents"]);

        // enqueue all the strips for each page
        for (auto& image: Dictionary(page["/Resources"]["/XObject"])) {
            if (!image.second.null()) {
                enqueue(image.second);
                enqueue(qpdf.newStream(image_transform_content));
            }
        }
    }

    enqueue(trimmed_trailer()["/Root"]);
}

void
impl::Writer::indicateProgress(bool decrement, bool finished)
{
    if (decrement) {
        --events_seen;
        return;
    }

    ++events_seen;

    if (!progress_reporter.get()) {
        return;
    }

    if (finished || events_seen >= next_progress_report) {
        int percentage =
            (finished ? 100
                 : next_progress_report == 0
                 ? 0
                 : std::min(99, 1 + ((100 * events_seen) / events_expected)));
        progress_reporter->reportProgress(percentage);
    }
    int increment = std::max(1, (events_expected / 100));
    while (events_seen >= next_progress_report) {
        next_progress_report += increment;
    }
}

void
QPDFWriter::registerProgressReporter(std::shared_ptr<ProgressReporter> pr)
{
    m->progress_reporter = pr;
}

void
impl::Writer::writeStandard()
{
    auto pp_md5 = pipeline_stack.popper();
    if (cfg.deterministic_id()) {
        pipeline_stack.activate_md5(pp_md5);
    }

    // Start writing

    writeHeader();
    write(cfg.extra_header_text());

    if (cfg.pclm()) {
        enqueueObjectsPCLm();
    } else {
        enqueueObjectsStandard();
    }

    // Now start walking queue, outputting each object.
    while (object_queue_front < object_queue.size()) {
        QPDFObjectHandle cur_object = object_queue.at(object_queue_front);
        ++object_queue_front;
        writeObject(cur_object);
    }

    // Write out the encryption dictionary, if any
    if (encryption) {
        writeEncryptionDictionary();
    }

    // Now write out xref.  next_objid is now the number of objects.
    qpdf_offset_t xref_offset = pipeline->getCount();
    if (object_stream_to_objects.empty()) {
        // Write regular cross-reference table
        writeXRefTable(t_normal, 0, next_objid - 1, next_objid);
    } else {
        // Write cross-reference stream.
        int xref_id = next_objid++;
        writeXRefStream(xref_id, xref_id, xref_offset, t_normal, 0, next_objid - 1, next_objid);
    }
    write("startxref\n").write(xref_offset).write("\n%%EOF\n");

    if (cfg.deterministic_id()) {
        QTC::TC(
            "qpdf",
            "QPDFWriter standard deterministic ID",
            object_stream_to_objects.empty() ? 0 : 1);
    }
}
