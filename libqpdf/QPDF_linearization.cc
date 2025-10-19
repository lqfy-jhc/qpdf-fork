// See doc/linearization.

#include <qpdf/QPDF_private.hh>

#include <qpdf/BitStream.hh>
#include <qpdf/BitWriter.hh>
#include <qpdf/InputSource_private.hh>
#include <qpdf/Pipeline_private.hh>
#include <qpdf/Pl_Buffer.hh>
#include <qpdf/Pl_Flate.hh>
#include <qpdf/Pl_String.hh>
#include <qpdf/QPDFExc.hh>
#include <qpdf/QPDFObjectHandle_private.hh>
#include <qpdf/QPDFWriter_private.hh>
#include <qpdf/QTC.hh>
#include <qpdf/QUtil.hh>
#include <qpdf/Util.hh>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

using namespace qpdf;
using namespace std::literals;

using Lin = QPDF::Doc::Linearization;

template <class T, class int_type>
static void
load_vector_int(
    BitStream& bit_stream, int nitems, std::vector<T>& vec, int bits_wanted, int_type T::* field)
{
    bool append = vec.empty();
    // nitems times, read bits_wanted from the given bit stream, storing results in the ith vector
    // entry.

    for (size_t i = 0; i < QIntC::to_size(nitems); ++i) {
        if (append) {
            vec.push_back(T());
        }
        vec.at(i).*field = bit_stream.getBitsInt(QIntC::to_size(bits_wanted));
    }
    util::assertion(
        std::cmp_equal(vec.size(), nitems), "vector has wrong size in load_vector_int" //
    );
    // The PDF spec says that each hint table starts at a byte boundary.  Each "row" actually must
    // start on a byte boundary.
    bit_stream.skipToNextByte();
}

template <class T>
static void
load_vector_vector(
    BitStream& bit_stream,
    int nitems1,
    std::vector<T>& vec1,
    int T::* nitems2,
    int bits_wanted,
    std::vector<int> T::* vec2)
{
    // nitems1 times, read nitems2 (from the ith element of vec1) items into the vec2 vector field
    // of the ith item of vec1.
    for (size_t i1 = 0; i1 < QIntC::to_size(nitems1); ++i1) {
        for (int i2 = 0; i2 < vec1.at(i1).*nitems2; ++i2) {
            (vec1.at(i1).*vec2).push_back(bit_stream.getBitsInt(QIntC::to_size(bits_wanted)));
        }
    }
    bit_stream.skipToNextByte();
}

Lin::ObjUser::ObjUser(user_e type) :
    ou_type(type)
{
    qpdf_expect(type == ou_root);
}

Lin::ObjUser::ObjUser(user_e type, size_t pageno) :
    ou_type(type),
    pageno(pageno)
{
    qpdf_expect(type == ou_page || type == ou_thumb);
}

Lin::ObjUser::ObjUser(user_e type, std::string const& key) :
    ou_type(type),
    key(key)
{
    qpdf_expect(type == ou_trailer_key || type == ou_root_key);
}

bool
Lin::ObjUser::operator<(ObjUser const& rhs) const
{
    if (ou_type < rhs.ou_type) {
        return true;
    }
    if (ou_type == rhs.ou_type) {
        if (pageno < rhs.pageno) {
            return true;
        }
        if (pageno == rhs.pageno) {
            return key < rhs.key;
        }
    }
    return false;
}

Lin::UpdateObjectMapsFrame::UpdateObjectMapsFrame(
    ObjUser const& ou, QPDFObjectHandle oh, bool top) :
    ou(ou),
    oh(oh),
    top(top)
{
}

void
QPDF::optimize(
    std::map<int, int> const& object_stream_data,
    bool allow_changes,
    std::function<int(QPDFObjectHandle&)> skip_stream_parameters)
{
    m->lin.optimize_internal(object_stream_data, allow_changes, skip_stream_parameters);
}

void
Lin::optimize(
    QPDFWriter::ObjTable const& obj, std::function<int(QPDFObjectHandle&)> skip_stream_parameters)
{
    optimize_internal(obj, true, skip_stream_parameters);
}

template <typename T>
void
Lin::optimize_internal(
    T const& object_stream_data,
    bool allow_changes,
    std::function<int(QPDFObjectHandle&)> skip_stream_parameters)
{
    if (!obj_user_to_objects_.empty()) {
        // already optimized
        return;
    }

    // The PDF specification indicates that /Outlines is supposed to be an indirect reference. Force
    // it to be so if it exists and is direct.  (This has been seen in the wild.)
    QPDFObjectHandle root = qpdf.getRoot();
    if (root.getKey("/Outlines").isDictionary()) {
        QPDFObjectHandle outlines = root.getKey("/Outlines");
        if (!outlines.isIndirect()) {
            root.replaceKey("/Outlines", qpdf.makeIndirectObject(outlines));
        }
    }

    // Traverse pages tree pushing all inherited resources down to the page level.  This also
    // initializes m->all_pages.
    m->pages.pushInheritedAttributesToPage(allow_changes, false);
    // Traverse pages

    size_t n = 0;
    for (auto const& page: m->pages) {
        updateObjectMaps(ObjUser(ObjUser::ou_page, n), page, skip_stream_parameters);
        ++n;
    }

    // Traverse document-level items
    for (auto const& [key, value]: m->trailer.as_dictionary()) {
        if (key == "/Root") {
            // handled separately
        } else {
            if (!value.null()) {
                updateObjectMaps(
                    ObjUser(ObjUser::ou_trailer_key, key), value, skip_stream_parameters);
            }
        }
    }

    for (auto const& [key, value]: root.as_dictionary()) {
        // Technically, /I keys from /Thread dictionaries are supposed to be handled separately, but
        // we are going to disregard that specification for now.  There is loads of evidence that
        // pdlin and Acrobat both disregard things like this from time to time, so this is almost
        // certain not to cause any problems.
        if (!value.null()) {
            updateObjectMaps(ObjUser(ObjUser::ou_root_key, key), value, skip_stream_parameters);
        }
    }

    ObjUser root_ou = ObjUser(ObjUser::ou_root);
    auto root_og = root.id_gen();
    obj_user_to_objects_[root_ou].insert(root_og);
    object_to_obj_users_[root_og].insert(root_ou);

    filterCompressedObjects(object_stream_data);
}

void
Lin::updateObjectMaps(
    ObjUser const& first_ou,
    QPDFObjectHandle first_oh,
    std::function<int(QPDFObjectHandle&)> skip_stream_parameters)
{
    QPDFObjGen::set visited;
    std::vector<UpdateObjectMapsFrame> pending;
    pending.emplace_back(first_ou, first_oh, true);
    // Traverse the object tree from this point taking care to avoid crossing page boundaries.
    std::unique_ptr<ObjUser> thumb_ou;
    while (!pending.empty()) {
        auto cur = pending.back();
        pending.pop_back();

        bool is_page_node = false;

        if (cur.oh.isDictionaryOfType("/Page")) {
            is_page_node = true;
            if (!cur.top) {
                continue;
            }
        }

        if (cur.oh.indirect()) {
            QPDFObjGen og(cur.oh.getObjGen());
            if (!visited.add(og)) {
                QTC::TC("qpdf", "QPDF opt loop detected");
                continue;
            }
            obj_user_to_objects_[cur.ou].insert(og);
            object_to_obj_users_[og].insert(cur.ou);
        }

        if (cur.oh.isArray()) {
            for (auto const& item: cur.oh.as_array()) {
                pending.emplace_back(cur.ou, item, false);
            }
        } else if (cur.oh.isDictionary() || cur.oh.isStream()) {
            QPDFObjectHandle dict = cur.oh;
            bool is_stream = cur.oh.isStream();
            int ssp = 0;
            if (is_stream) {
                dict = cur.oh.getDict();
                if (skip_stream_parameters) {
                    ssp = skip_stream_parameters(cur.oh);
                }
            }

            for (auto& [key, value]: dict.as_dictionary()) {
                if (value.null()) {
                    continue;
                }

                if (is_page_node && (key == "/Thumb")) {
                    // Traverse page thumbnail dictionaries as a special case. There can only ever
                    // be one /Thumb key on a page, and we see at most one page node per call.
                    thumb_ou = std::make_unique<ObjUser>(ObjUser::ou_thumb, cur.ou.pageno);
                    pending.emplace_back(*thumb_ou, dict.getKey(key), false);
                } else if (is_page_node && (key == "/Parent")) {
                    // Don't traverse back up the page tree
                } else if (
                    ((ssp >= 1) && (key == "/Length")) ||
                    ((ssp >= 2) && ((key == "/Filter") || (key == "/DecodeParms")))) {
                    // Don't traverse into stream parameters that we are not going to write.
                } else {
                    pending.emplace_back(cur.ou, value, false);
                }
            }
        }
    }
}

void
Lin::filterCompressedObjects(std::map<int, int> const& object_stream_data)
{
    if (object_stream_data.empty()) {
        return;
    }

    // Transform object_to_obj_users and obj_user_to_objects so that they refer only to uncompressed
    // objects.  If something is a user of a compressed object, then it is really a user of the
    // object stream that contains it.

    std::map<ObjUser, std::set<QPDFObjGen>> t_obj_user_to_objects;
    std::map<QPDFObjGen, std::set<ObjUser>> t_object_to_obj_users;

    for (auto const& [ou, ogs]: obj_user_to_objects_) {
        for (auto const& og: ogs) {
            auto i2 = object_stream_data.find(og.getObj());
            if (i2 == object_stream_data.end()) {
                t_obj_user_to_objects[ou].insert(og);
            } else {
                t_obj_user_to_objects[ou].insert({i2->second, 0});
            }
        }
    }

    for (auto const& [og, ous]: object_to_obj_users_) {
        for (auto const& ou: ous) {
            auto i2 = object_stream_data.find(og.getObj());
            if (i2 == object_stream_data.end()) {
                t_object_to_obj_users[og].insert(ou);
            } else {
                t_object_to_obj_users[{i2->second, 0}].insert(ou);
            }
        }
    }

    obj_user_to_objects_ = std::move(t_obj_user_to_objects);
    object_to_obj_users_ = std::move(t_object_to_obj_users);
}

void
Lin::filterCompressedObjects(QPDFWriter::ObjTable const& obj)
{
    if (obj.getStreamsEmpty()) {
        return;
    }

    // Transform object_to_obj_users and obj_user_to_objects so that they refer only to uncompressed
    // objects.  If something is a user of a compressed object, then it is really a user of the
    // object stream that contains it.

    std::map<ObjUser, std::set<QPDFObjGen>> t_obj_user_to_objects;
    std::map<QPDFObjGen, std::set<ObjUser>> t_object_to_obj_users;

    for (auto const& [ou, ogs]: obj_user_to_objects_) {
        for (auto const& og: ogs) {
            if (obj.contains(og)) {
                if (auto const& i2 = obj[og].object_stream; i2 <= 0) {
                    t_obj_user_to_objects[ou].insert(og);
                } else {
                    t_obj_user_to_objects[ou].insert(QPDFObjGen(i2, 0));
                }
            }
        }
    }

    for (auto const& [og, ous]: object_to_obj_users_) {
        if (obj.contains(og)) {
            // Loop over obj_users.
            for (auto const& ou: ous) {
                if (auto i2 = obj[og].object_stream; i2 <= 0) {
                    t_object_to_obj_users[og].insert(ou);
                } else {
                    t_object_to_obj_users[{i2, 0}].insert(ou);
                }
            }
        }
    }

    obj_user_to_objects_ = std::move(t_obj_user_to_objects);
    object_to_obj_users_ = std::move(t_object_to_obj_users);
}

void
Lin::linearizationWarning(std::string_view msg)
{
    linearization_warnings_ = true;
    warn(qpdf_e_linearization, "", 0, std::string(msg));
}

bool
QPDF::checkLinearization()
{
    return m->lin.check();
}

bool
Lin::check()
{
    try {
        readLinearizationData();
        checkLinearizationInternal();
        return !linearization_warnings_;
    } catch (std::runtime_error& e) {
        linearizationWarning(
            "error encountered while checking linearization data: " + std::string(e.what()));
        return false;
    }
}

bool
QPDF::isLinearized()
{
    return m->lin.linearized();
}

bool
Lin::linearized()
{
    // If the first object in the file is a dictionary with a suitable /Linearized key and has an /L
    // key that accurately indicates the file size, initialize m->lindict and return true.

    // A linearized PDF spec's first object will be contained within the first 1024 bytes of the
    // file and will be a dictionary with a valid /Linearized key.  This routine looks for that and
    // does no additional validation.

    // The PDF spec says the linearization dictionary must be completely contained within the first
    // 1024 bytes of the file. Add a byte for a null terminator.
    auto buffer = m->file->read(1024, 0);
    size_t pos = 0;
    while (true) {
        // Find a digit or end of buffer
        pos = buffer.find_first_of("0123456789"sv, pos);
        if (pos == std::string::npos) {
            return false;
        }
        // Seek to the digit. Then skip over digits for a potential
        // next iteration.
        m->file->seek(toO(pos), SEEK_SET);

        auto t1 = m->objects.readToken(*m->file, 20);
        if (!(t1.isInteger() && m->objects.readToken(*m->file, 6).isInteger() &&
              m->objects.readToken(*m->file, 4).isWord("obj"))) {
            pos = buffer.find_first_not_of("0123456789"sv, pos);
            if (pos == std::string::npos) {
                return false;
            }
            continue;
        }

        Dictionary candidate = qpdf.getObject(toI(QUtil::string_to_ll(t1.getValue().data())), 0);
        auto linkey = candidate["/Linearized"];
        if (!(linkey.isNumber() && toI(floor(linkey.getNumericValue())) == 1)) {
            return false;
        }

        m->file->seek(0, SEEK_END);
        Integer L = candidate["/L"];
        if (L != m->file->tell()) {
            return false;
        }
        linp_.file_size = L;
        lindict_ = candidate;
        return true;
    }
}

void
Lin::readLinearizationData()
{
    util::assertion(
        linearized(), "called readLinearizationData for file that is not linearized" //
    );

    // This function throws an exception (which is trapped by checkLinearization()) for any errors
    // that prevent loading.

    // /L is read and stored in linp by isLinearized()
    Array H = lindict_["/H"]; // hint table offset/length for primary and overflow hint tables
    auto H_size = H.size();
    Integer H_0 = H[0]; // hint table offset
    Integer H_1 = H[1]; // hint table length
    Integer H_2 = H[2]; // hint table offset for overflow hint table
    Integer H_3 = H[3]; // hint table length for overflow hint table
    Integer O = lindict_["/O"];
    Integer E = lindict_["/E"];
    Integer N = lindict_["/N"];
    Integer T = lindict_["/T"];
    auto P_oh = lindict_["/P"];
    Integer P = P_oh; // first page number
    QTC::TC("qpdf", "QPDF P absent in lindict", P ? 0 : 1);

    no_ci_stop_if(
        !(H && O && E && N && T && (P || P_oh.null())),
        "some keys in linearization dictionary are of the wrong type",
        "linearization dictionary" //
    );

    no_ci_stop_if(
        !(H_size == 2 || H_size == 4),
        "H has the wrong number of items",
        "linearization dictionary" //
    );

    no_ci_stop_if(
        !(H_0 && H_1 && (H_size == 2 || (H_2 && H_3))),
        "some H items are of the wrong type",
        "linearization dictionary" //
    );

    // Store linearization parameter data

    // Various places in the code use linp.npages, which is initialized from N, to pre-allocate
    // memory, so make sure it's accurate and bail right now if it's not.
    no_ci_stop_if(
        N != pages.size(),
        "/N does not match number of pages",
        "linearization dictionary" //
    );

    // file_size initialized by isLinearized()
    linp_.first_page_object = O;
    linp_.first_page_end = E;
    linp_.npages = N;
    linp_.xref_zero_offset = T;
    linp_.first_page = P ? P : 0;
    linp_.H_offset = H_0;
    linp_.H_length = H_1;

    // Read hint streams

    Pl_Buffer pb("hint buffer");
    auto H0 = readHintStream(pb, H_0, H_1);
    if (H_2) {
        (void)readHintStream(pb, H_2, H_3);
    }

    // PDF 1.4 hint tables that we ignore:

    //  /T    thumbnail
    //  /A    thread information
    //  /E    named destination
    //  /V    interactive form
    //  /I    information dictionary
    //  /C    logical structure
    //  /L    page label

    // Individual hint table offsets
    Integer HS = H0["/S"]; // shared object
    Integer HO = H0["/O"]; // outline

    auto hbp = pb.getBufferSharedPointer();
    Buffer* hb = hbp.get();
    unsigned char const* h_buf = hb->getBuffer();
    size_t h_size = hb->getSize();

    readHPageOffset(BitStream(h_buf, h_size));

    size_t HSi = HS;
    if (HSi < 0 || HSi >= h_size) {
        throw damagedPDF("linearization hint table", "/S (shared object) offset is out of bounds");
    }
    readHSharedObject(BitStream(h_buf + HSi, h_size - HSi));

    if (HO) {
        no_ci_stop_if(
            HO < 0 || HO >= h_size,
            "/O (outline) offset is out of bounds",
            "linearization dictionary" //
        );
        size_t HOi = HO;
        readHGeneric(BitStream(h_buf + HO, h_size - HOi), outline_hints_);
    }
}

Dictionary
Lin::readHintStream(Pipeline& pl, qpdf_offset_t offset, size_t length)
{
    auto H = m->objects.readObjectAtOffset(offset, "linearization hint stream", false);
    ObjCache& oc = m->obj_cache[H];
    qpdf_offset_t min_end_offset = oc.end_before_space;
    qpdf_offset_t max_end_offset = oc.end_after_space;
    no_ci_stop_if(
        !H.isStream(), "hint table is not a stream", "linearization dictionary" //
    );

    Dictionary Hdict = H.getDict();

    // Some versions of Acrobat make /Length indirect and place it immediately after the stream,
    // increasing length to cover it, even though the specification says all objects in the
    // linearization parameter dictionary must be direct.  We have to get the file position of the
    // end of length in this case.
    if (Hdict["/Length"].indirect()) {
        ObjCache& oc2 = m->obj_cache[Hdict["/Length"]];
        min_end_offset = oc2.end_before_space;
        max_end_offset = oc2.end_after_space;
    } else {
        QTC::TC("qpdf", "QPDF hint table length direct");
    }
    qpdf_offset_t computed_end = offset + toO(length);
    no_ci_stop_if(
        computed_end < min_end_offset || computed_end > max_end_offset,
        "hint table length mismatch (expected = " + std::to_string(computed_end) + "; actual = " +
            std::to_string(min_end_offset) + ".." + std::to_string(max_end_offset) + ")",
        "linearization dictionary" //
    );
    H.pipeStreamData(&pl, 0, qpdf_dl_specialized);
    return Hdict;
}

void
Lin::readHPageOffset(BitStream h)
{
    // All comments referring to the PDF spec refer to the spec for version 1.4.

    HPageOffset& t = page_offset_hints_;

    t.min_nobjects = h.getBitsInt(32);               // 1
    t.first_page_offset = h.getBitsInt(32);          // 2
    t.nbits_delta_nobjects = h.getBitsInt(16);       // 3
    t.min_page_length = h.getBitsInt(32);            // 4
    t.nbits_delta_page_length = h.getBitsInt(16);    // 5
    t.min_content_offset = h.getBitsInt(32);         // 6
    t.nbits_delta_content_offset = h.getBitsInt(16); // 7
    t.min_content_length = h.getBitsInt(32);         // 8
    t.nbits_delta_content_length = h.getBitsInt(16); // 9
    t.nbits_nshared_objects = h.getBitsInt(16);      // 10
    t.nbits_shared_identifier = h.getBitsInt(16);    // 11
    t.nbits_shared_numerator = h.getBitsInt(16);     // 12
    t.shared_denominator = h.getBitsInt(16);         // 13

    std::vector<HPageOffsetEntry>& entries = t.entries;
    entries.clear();
    int nitems = toI(linp_.npages);
    load_vector_int(h, nitems, entries, t.nbits_delta_nobjects, &HPageOffsetEntry::delta_nobjects);
    load_vector_int(
        h, nitems, entries, t.nbits_delta_page_length, &HPageOffsetEntry::delta_page_length);
    load_vector_int(
        h, nitems, entries, t.nbits_nshared_objects, &HPageOffsetEntry::nshared_objects);
    load_vector_vector(
        h,
        nitems,
        entries,
        &HPageOffsetEntry::nshared_objects,
        t.nbits_shared_identifier,
        &HPageOffsetEntry::shared_identifiers);
    load_vector_vector(
        h,
        nitems,
        entries,
        &HPageOffsetEntry::nshared_objects,
        t.nbits_shared_numerator,
        &HPageOffsetEntry::shared_numerators);
    load_vector_int(
        h, nitems, entries, t.nbits_delta_content_offset, &HPageOffsetEntry::delta_content_offset);
    load_vector_int(
        h, nitems, entries, t.nbits_delta_content_length, &HPageOffsetEntry::delta_content_length);
}

void
Lin::readHSharedObject(BitStream h)
{
    HSharedObject& t = shared_object_hints_;

    t.first_shared_obj = h.getBitsInt(32);         // 1
    t.first_shared_offset = h.getBitsInt(32);      // 2
    t.nshared_first_page = h.getBitsInt(32);       // 3
    t.nshared_total = h.getBitsInt(32);            // 4
    t.nbits_nobjects = h.getBitsInt(16);           // 5
    t.min_group_length = h.getBitsInt(32);         // 6
    t.nbits_delta_group_length = h.getBitsInt(16); // 7

    QTC::TC(
        "qpdf",
        "QPDF lin nshared_total > nshared_first_page",
        (t.nshared_total > t.nshared_first_page) ? 1 : 0);

    std::vector<HSharedObjectEntry>& entries = t.entries;
    entries.clear();
    int nitems = t.nshared_total;
    load_vector_int(
        h, nitems, entries, t.nbits_delta_group_length, &HSharedObjectEntry::delta_group_length);
    load_vector_int(h, nitems, entries, 1, &HSharedObjectEntry::signature_present);
    for (size_t i = 0; i < toS(nitems); ++i) {
        if (entries.at(i).signature_present) {
            // Skip 128-bit MD5 hash.  These are not supported by acrobat, so they should probably
            // never be there.  We have no test case for this.
            for (int j = 0; j < 4; ++j) {
                (void)h.getBits(32);
            }
        }
    }
    load_vector_int(h, nitems, entries, t.nbits_nobjects, &HSharedObjectEntry::nobjects_minus_one);
}

void
Lin::readHGeneric(BitStream h, HGeneric& t)
{
    t.first_object = h.getBitsInt(32);        // 1
    t.first_object_offset = h.getBitsInt(32); // 2
    t.nobjects = h.getBitsInt(32);            // 3
    t.group_length = h.getBitsInt(32);        // 4
}

void
Lin::checkLinearizationInternal()
{
    // All comments referring to the PDF spec refer to the spec for version 1.4.

    // Check all values in linearization parameter dictionary

    LinParameters& p = linp_;

    // L: file size in bytes -- checked by isLinearized

    // O: object number of first page
    auto const& all_pages = pages.all();
    if (p.first_page_object != all_pages.at(0).getObjectID()) {
        linearizationWarning("first page object (/O) mismatch");
    }

    // N: number of pages
    size_t npages = all_pages.size();
    if (std::cmp_not_equal(p.npages, npages)) {
        // Not tested in the test suite
        linearizationWarning("page count (/N) mismatch");
    }

    int i = 0;
    for (auto const& page: all_pages) {
        if (m->xref_table[page].getType() == 2) {
            linearizationWarning(
                "page dictionary for page " + std::to_string(i) + " is compressed");
        }
        ++i;
    }

    // T: offset of whitespace character preceding xref entry for object 0
    m->file->seek(p.xref_zero_offset, SEEK_SET);
    while (true) {
        char ch;
        m->file->read(&ch, 1);
        if (!(ch == ' ' || ch == '\r' || ch == '\n')) {
            m->file->seek(-1, SEEK_CUR);
            break;
        }
    }
    if (m->file->tell() != objects.first_xref_item_offset()) {
        linearizationWarning(
            "space before first xref item (/T) mismatch (computed = " +
            std::to_string(objects.first_xref_item_offset()) +
            "; file = " + std::to_string(m->file->tell()));
    }

    // P: first page number -- Implementation note 124 says Acrobat ignores this value, so we will
    // too.

    // Check numbering of compressed objects in each xref section. For linearized files, all
    // compressed objects are supposed to be at the end of the containing xref section if any object
    // streams are in use.

    if (objects.uncompressed_after_compressed()) {
        linearizationWarning(
            "linearized file contains an uncompressed object after a compressed "
            "one in a cross-reference stream");
    }

    // Further checking requires optimization and order calculation. Don't allow optimization to
    // make changes.  If it has to, then the file is not properly linearized.  We use the xref table
    // to figure out which objects are compressed and which are uncompressed.
    { // local scope
        std::map<int, int> object_stream_data;
        for (auto const& [og, entry]: m->xref_table) {
            if (entry.getType() == 2) {
                object_stream_data[og.getObj()] = entry.getObjStreamNumber();
            }
        }
        optimize_internal(object_stream_data, false, nullptr);
        calculateLinearizationData(object_stream_data);
    }

    // E: offset of end of first page -- Implementation note 123 says Acrobat includes on extra
    // object here by mistake.  pdlin fails to place thumbnail images in section 9, so when
    // thumbnails are present, it also gets the wrong value for /E.  It also doesn't count outlines
    // here when it should even though it places them in part 6.  This code fails to put thread
    // information dictionaries in part 9, so it actually gets the wrong value for E when threads
    // are present.  In that case, it would probably agree with pdlin.  As of this writing, the test
    // suite doesn't contain any files with threads.

    no_ci_stop_if(
        part6_.empty(), "linearization part 6 unexpectedly empty" //
    );
    qpdf_offset_t min_E = -1;
    qpdf_offset_t max_E = -1;
    for (auto const& oh: part6_) {
        QPDFObjGen og(oh.getObjGen());
        // All objects have to have been dereferenced to be classified.
        util::assertion(m->obj_cache.contains(og), "linearization part6 object not in cache");
        ObjCache const& oc = m->obj_cache[og];
        min_E = std::max(min_E, oc.end_before_space);
        max_E = std::max(max_E, oc.end_after_space);
    }
    if (p.first_page_end < min_E || p.first_page_end > max_E) {
        linearizationWarning(
            "end of first page section (/E) mismatch: /E = " + std::to_string(p.first_page_end) +
            "; computed = " + std::to_string(min_E) + ".." + std::to_string(max_E));
    }

    // Check hint tables

    std::map<int, int> shared_idx_to_obj;
    checkHSharedObject(all_pages, shared_idx_to_obj);
    checkHPageOffset(all_pages, shared_idx_to_obj);
    checkHOutlines();
}

qpdf_offset_t
Lin::maxEnd(ObjUser const& ou)
{
    no_ci_stop_if(
        !obj_user_to_objects_.contains(ou),
        "no entry in object user table for requested object user" //
    );

    qpdf_offset_t end = 0;
    for (auto const& og: obj_user_to_objects_[ou]) {
        no_ci_stop_if(
            !m->obj_cache.contains(og), "unknown object referenced in object user table" //
        );
        end = std::max(end, m->obj_cache[og].end_after_space);
    }
    return end;
}

qpdf_offset_t
Lin::getLinearizationOffset(QPDFObjGen og)
{
    QPDFXRefEntry const& entry = m->xref_table[og];
    auto typ = entry.getType();
    if (typ == 1) {
        return entry.getOffset();
    }
    no_ci_stop_if(
        typ != 2, "getLinearizationOffset called for xref entry not of type 1 or 2" //
    );
    // For compressed objects, return the offset of the object stream that contains them.
    return getLinearizationOffset({entry.getObjStreamNumber(), 0});
}

QPDFObjectHandle
Lin::getUncompressedObject(QPDFObjectHandle& obj, std::map<int, int> const& object_stream_data)
{
    if (obj.null() || !object_stream_data.contains(obj.getObjectID())) {
        return obj;
    }
    return qpdf.getObject((*(object_stream_data.find(obj.getObjectID()))).second, 0);
}

QPDFObjectHandle
Lin::getUncompressedObject(QPDFObjectHandle& oh, QPDFWriter::ObjTable const& obj)
{
    if (obj.contains(oh)) {
        if (auto id = obj[oh].object_stream; id > 0) {
            return oh.null() ? oh : qpdf.getObject(id, 0);
        }
    }
    return oh;
}

int
Lin::lengthNextN(int first_object, int n)
{
    int length = 0;
    for (int i = 0; i < n; ++i) {
        QPDFObjGen og(first_object + i, 0);
        if (m->xref_table.contains(og)) {
            no_ci_stop_if(
                !m->obj_cache.contains(og),
                "found unknown object while calculating length for linearization data" //
            );

            length += toI(m->obj_cache[og].end_after_space - getLinearizationOffset(og));
        } else {
            linearizationWarning(
                "no xref table entry for " + std::to_string(first_object + i) + " 0");
        }
    }
    return length;
}

void
Lin::checkHPageOffset(
    std::vector<QPDFObjectHandle> const& pages, std::map<int, int>& shared_idx_to_obj)
{
    // Implementation note 126 says Acrobat always sets delta_content_offset and
    // delta_content_length in the page offset header dictionary to 0.  It also states that
    // min_content_offset in the per-page information is always 0, which is an incorrect value.

    // Implementation note 127 explains that Acrobat always sets item 8 (min_content_length) to
    // zero, item 9 (nbits_delta_content_length) to the value of item 5 (nbits_delta_page_length),
    // and item 7 of each per-page hint table (delta_content_length) to item 2 (delta_page_length)
    // of that entry.  Acrobat ignores these values when reading files.

    // Empirically, it also seems that Acrobat sometimes puts items under a page's /Resources
    // dictionary in with shared objects even when they are private.

    size_t npages = pages.size();
    qpdf_offset_t table_offset = adjusted_offset(page_offset_hints_.first_page_offset);
    QPDFObjGen first_page_og(pages.at(0).getObjGen());
    if (!m->xref_table.contains(first_page_og)) {
        stopOnError("supposed first page object is not known");
    }
    qpdf_offset_t offset = getLinearizationOffset(first_page_og);
    if (table_offset != offset) {
        linearizationWarning("first page object offset mismatch");
    }

    for (size_t pageno = 0; pageno < npages; ++pageno) {
        QPDFObjGen page_og(pages.at(pageno).getObjGen());
        int first_object = page_og.getObj();
        if (!m->xref_table.contains(page_og)) {
            stopOnError("unknown object in page offset hint table");
        }
        offset = getLinearizationOffset(page_og);

        HPageOffsetEntry& he = page_offset_hints_.entries.at(pageno);
        CHPageOffsetEntry& ce = c_page_offset_data_.entries.at(pageno);
        int h_nobjects = he.delta_nobjects + page_offset_hints_.min_nobjects;
        if (h_nobjects != ce.nobjects) {
            // This happens with pdlin when there are thumbnails.
            linearizationWarning(
                "object count mismatch for page " + std::to_string(pageno) + ": hint table = " +
                std::to_string(h_nobjects) + "; computed = " + std::to_string(ce.nobjects));
        }

        // Use value for number of objects in hint table rather than computed value if there is a
        // discrepancy.
        int length = lengthNextN(first_object, h_nobjects);
        int h_length = toI(he.delta_page_length + page_offset_hints_.min_page_length);
        if (length != h_length) {
            // This condition almost certainly indicates a bad hint table or a bug in this code.
            linearizationWarning(
                "page length mismatch for page " + std::to_string(pageno) + ": hint table = " +
                std::to_string(h_length) + "; computed length = " + std::to_string(length) +
                " (offset = " + std::to_string(offset) + ")");
        }

        offset += h_length;

        // Translate shared object indexes to object numbers.
        std::set<int> hint_shared;
        std::set<int> computed_shared;

        if (pageno == 0 && he.nshared_objects > 0) {
            // pdlin and Acrobat both do this even though the spec states clearly and unambiguously
            // that they should not.
            linearizationWarning("page 0 has shared identifier entries");
        }

        for (size_t i = 0; i < toS(he.nshared_objects); ++i) {
            int idx = he.shared_identifiers.at(i);
            no_ci_stop_if(
                !shared_idx_to_obj.contains(idx),
                "unable to get object for item in shared objects hint table");

            hint_shared.insert(shared_idx_to_obj[idx]);
        }

        for (size_t i = 0; i < toS(ce.nshared_objects); ++i) {
            int idx = ce.shared_identifiers.at(i);
            no_ci_stop_if(
                idx >= c_shared_object_data_.nshared_total,
                "index out of bounds for shared object hint table" //
            );

            int obj = c_shared_object_data_.entries.at(toS(idx)).object;
            computed_shared.insert(obj);
        }

        for (int iter: hint_shared) {
            if (!computed_shared.contains(iter)) {
                // pdlin puts thumbnails here even though it shouldn't
                linearizationWarning(
                    "page " + std::to_string(pageno) + ": shared object " + std::to_string(iter) +
                    ": in hint table but not computed list");
            }
        }

        for (int iter: computed_shared) {
            if (!hint_shared.contains(iter)) {
                // Acrobat does not put some things including at least built-in fonts and procsets
                // here, at least in some cases.
                linearizationWarning(
                    ("page " + std::to_string(pageno) + ": shared object " + std::to_string(iter) +
                     ": in computed list but not hint table"));
            }
        }
    }
}

void
Lin::checkHSharedObject(std::vector<QPDFObjectHandle> const& pages, std::map<int, int>& idx_to_obj)
{
    // Implementation note 125 says shared object groups always contain only one object.
    // Implementation note 128 says that Acrobat always nbits_nobjects to zero.  Implementation note
    // 130 says that Acrobat does not support more than one shared object per group.  These are all
    // consistent.

    // Implementation note 129 states that MD5 signatures are not implemented in Acrobat, so
    // signature_present must always be zero.

    // Implementation note 131 states that first_shared_obj and first_shared_offset have meaningless
    // values for single-page files.

    // Empirically, Acrobat and pdlin generate incorrect values for these whenever there are no
    // shared objects not referenced by the first page (i.e., nshared_total == nshared_first_page).

    HSharedObject& so = shared_object_hints_;
    if (so.nshared_total < so.nshared_first_page) {
        linearizationWarning("shared object hint table: ntotal < nfirst_page");
    } else {
        // The first nshared_first_page objects are consecutive objects starting with the first page
        // object.  The rest are consecutive starting from the first_shared_obj object.
        int cur_object = pages.at(0).getObjectID();
        for (int i = 0; i < so.nshared_total; ++i) {
            if (i == so.nshared_first_page) {
                QTC::TC("qpdf", "QPDF lin check shared past first page");
                if (part8_.empty()) {
                    linearizationWarning("part 8 is empty but nshared_total > nshared_first_page");
                } else {
                    int obj = part8_.at(0).getObjectID();
                    if (obj != so.first_shared_obj) {
                        linearizationWarning(
                            "first shared object number mismatch: hint table = " +
                            std::to_string(so.first_shared_obj) +
                            "; computed = " + std::to_string(obj));
                    }
                }

                cur_object = so.first_shared_obj;

                QPDFObjGen og(cur_object, 0);
                if (!m->xref_table.contains(og)) {
                    stopOnError("unknown object in shared object hint table");
                }
                qpdf_offset_t offset = getLinearizationOffset(og);
                qpdf_offset_t h_offset = adjusted_offset(so.first_shared_offset);
                if (offset != h_offset) {
                    linearizationWarning(
                        "first shared object offset mismatch: hint table = " +
                        std::to_string(h_offset) + "; computed = " + std::to_string(offset));
                }
            }

            idx_to_obj[i] = cur_object;
            HSharedObjectEntry& se = so.entries.at(toS(i));
            int nobjects = se.nobjects_minus_one + 1;
            int length = lengthNextN(cur_object, nobjects);
            int h_length = so.min_group_length + se.delta_group_length;
            if (length != h_length) {
                linearizationWarning(
                    "shared object " + std::to_string(i) + " length mismatch: hint table = " +
                    std::to_string(h_length) + "; computed = " + std::to_string(length));
            }
            cur_object += nobjects;
        }
    }
}

void
Lin::checkHOutlines()
{
    // Empirically, Acrobat generates the correct value for the object number but incorrectly stores
    // the next object number's offset as the offset, at least when outlines appear in part 6.  It
    // also generates an incorrect value for length (specifically, the length that would cover the
    // correct number of objects from the wrong starting place).  pdlin appears to generate correct
    // values in those cases.

    if (c_outline_data_.nobjects == outline_hints_.nobjects) {
        if (c_outline_data_.nobjects == 0) {
            return;
        }

        if (c_outline_data_.first_object == outline_hints_.first_object) {
            // Check length and offset.  Acrobat gets these wrong.
            QPDFObjectHandle outlines = qpdf.getRoot().getKey("/Outlines");
            if (!outlines.isIndirect()) {
                // This case is not exercised in test suite since not permitted by the spec, but if
                // this does occur, the code below would fail.
                linearizationWarning("/Outlines key of root dictionary is not indirect");
                return;
            }
            QPDFObjGen og(outlines.getObjGen());
            no_ci_stop_if(
                !m->xref_table.contains(og), "unknown object in outlines hint table" //
            );
            qpdf_offset_t offset = getLinearizationOffset(og);
            ObjUser ou(ObjUser::ou_root_key, "/Outlines");
            int length = toI(maxEnd(ou) - offset);
            qpdf_offset_t table_offset = adjusted_offset(outline_hints_.first_object_offset);
            if (offset != table_offset) {
                linearizationWarning(
                    "incorrect offset in outlines table: hint table = " +
                    std::to_string(table_offset) + "; computed = " + std::to_string(offset));
            }
            int table_length = outline_hints_.group_length;
            if (length != table_length) {
                linearizationWarning(
                    "incorrect length in outlines table: hint table = " +
                    std::to_string(table_length) + "; computed = " + std::to_string(length));
            }
        } else {
            linearizationWarning("incorrect first object number in outline hints table.");
        }
    } else {
        linearizationWarning("incorrect object count in outline hint table");
    }
}

void
QPDF::showLinearizationData()
{
    m->lin.show_data();
}

void
Lin::show_data()
{
    try {
        readLinearizationData();
        checkLinearizationInternal();
        dumpLinearizationDataInternal();
    } catch (QPDFExc& e) {
        linearizationWarning(e.what());
    }
}

void
Lin::dumpLinearizationDataInternal()
{
    auto& info = *cf.log()->getInfo();

    info << m->file->getName() << ": linearization data:\n\n";

    info << "file_size: " << linp_.file_size << "\n"
         << "first_page_object: " << linp_.first_page_object << "\n"
         << "first_page_end: " << linp_.first_page_end << "\n"
         << "npages: " << linp_.npages << "\n"
         << "xref_zero_offset: " << linp_.xref_zero_offset << "\n"
         << "first_page: " << linp_.first_page << "\n"
         << "H_offset: " << linp_.H_offset << "\n"
         << "H_length: " << linp_.H_length << "\n"
         << "\n";

    info << "Page Offsets Hint Table\n\n";
    dumpHPageOffset();
    info << "\nShared Objects Hint Table\n\n";
    dumpHSharedObject();

    if (outline_hints_.nobjects > 0) {
        info << "\nOutlines Hint Table\n\n";
        dumpHGeneric(outline_hints_);
    }
}

qpdf_offset_t
Lin::adjusted_offset(qpdf_offset_t offset)
{
    // All offsets >= H_offset have to be increased by H_length since all hint table location values
    // disregard the hint table itself.
    if (offset >= linp_.H_offset) {
        return offset + linp_.H_length;
    }
    return offset;
}

void
Lin::dumpHPageOffset()
{
    auto& info = *cf.log()->getInfo();
    HPageOffset& t = page_offset_hints_;
    info << "min_nobjects: " << t.min_nobjects << "\n"
         << "first_page_offset: " << adjusted_offset(t.first_page_offset) << "\n"
         << "nbits_delta_nobjects: " << t.nbits_delta_nobjects << "\n"
         << "min_page_length: " << t.min_page_length << "\n"
         << "nbits_delta_page_length: " << t.nbits_delta_page_length << "\n"
         << "min_content_offset: " << t.min_content_offset << "\n"
         << "nbits_delta_content_offset: " << t.nbits_delta_content_offset << "\n"
         << "min_content_length: " << t.min_content_length << "\n"
         << "nbits_delta_content_length: " << t.nbits_delta_content_length << "\n"
         << "nbits_nshared_objects: " << t.nbits_nshared_objects << "\n"
         << "nbits_shared_identifier: " << t.nbits_shared_identifier << "\n"
         << "nbits_shared_numerator: " << t.nbits_shared_numerator << "\n"
         << "shared_denominator: " << t.shared_denominator << "\n";

    for (size_t i1 = 0; i1 < linp_.npages; ++i1) {
        HPageOffsetEntry& pe = t.entries.at(i1);
        info << "Page " << i1 << ":\n"
             << "  nobjects: " << pe.delta_nobjects + t.min_nobjects << "\n"
             << "  length: " << pe.delta_page_length + t.min_page_length
             << "\n"
             // content offset is relative to page, not file
             << "  content_offset: " << pe.delta_content_offset + t.min_content_offset << "\n"
             << "  content_length: " << pe.delta_content_length + t.min_content_length << "\n"
             << "  nshared_objects: " << pe.nshared_objects << "\n";
        for (size_t i2 = 0; i2 < toS(pe.nshared_objects); ++i2) {
            info << "    identifier " << i2 << ": " << pe.shared_identifiers.at(i2) << "\n";
            info << "    numerator " << i2 << ": " << pe.shared_numerators.at(i2) << "\n";
        }
    }
}

void
Lin::dumpHSharedObject()
{
    auto& info = *cf.log()->getInfo();
    HSharedObject& t = shared_object_hints_;
    info << "first_shared_obj: " << t.first_shared_obj << "\n"
         << "first_shared_offset: " << adjusted_offset(t.first_shared_offset) << "\n"
         << "nshared_first_page: " << t.nshared_first_page << "\n"
         << "nshared_total: " << t.nshared_total << "\n"
         << "nbits_nobjects: " << t.nbits_nobjects << "\n"
         << "min_group_length: " << t.min_group_length << "\n"
         << "nbits_delta_group_length: " << t.nbits_delta_group_length << "\n";

    for (size_t i = 0; i < toS(t.nshared_total); ++i) {
        HSharedObjectEntry& se = t.entries.at(i);
        info << "Shared Object " << i << ":\n"
             << "  group length: " << se.delta_group_length + t.min_group_length << "\n";
        // PDF spec says signature present nobjects_minus_one are always 0, so print them only if
        // they have a non-zero value.
        if (se.signature_present) {
            info << "  signature present\n";
        }
        if (se.nobjects_minus_one != 0) {
            info << "  nobjects: " << se.nobjects_minus_one + 1 << "\n";
        }
    }
}

void
Lin::dumpHGeneric(HGeneric& t)
{
    *cf.log()->getInfo() << "first_object: " << t.first_object << "\n"
                         << "first_object_offset: " << adjusted_offset(t.first_object_offset)
                         << "\n"
                         << "nobjects: " << t.nobjects << "\n"
                         << "group_length: " << t.group_length << "\n";
}

template <typename T>
void
Lin::calculateLinearizationData(T const& object_stream_data)
{
    // This function calculates the ordering of objects, divides them into the appropriate parts,
    // and computes some values for the linearization parameter dictionary and hint tables.  The
    // file must be optimized (via calling optimize()) prior to calling this function.  Note that
    // actual offsets and lengths are not computed here, but anything related to object ordering is.

    util::assertion(
        !object_to_obj_users_.empty(),
        "INTERNAL ERROR: QPDF::calculateLinearizationData called before optimize()" //
    );
    // Note that we can't call optimize here because we don't know whether it should be called
    // with or without allow changes.

    // Separate objects into the categories sufficient for us to determine which part of the
    // linearized file should contain the object.  This categorization is useful for other purposes
    // as well.  Part numbers refer to version 1.4 of the PDF spec.

    // Parts 1, 3, 5, 10, and 11 don't contain any objects from the original file (except the
    // trailer dictionary in part 11).

    // Part 4 is the document catalog (root) and the following root keys: /ViewerPreferences,
    // /PageMode, /Threads, /OpenAction, /AcroForm, /Encrypt.  Note that Thread information
    // dictionaries are supposed to appear in part 9, but we are disregarding that recommendation
    // for now.

    // Part 6 is the first page section.  It includes all remaining objects referenced by the first
    // page including shared objects but not including thumbnails.  Additionally, if /PageMode is
    // /Outlines, then information from /Outlines also appears here.

    // Part 7 contains remaining objects private to pages other than the first page.

    // Part 8 contains all remaining shared objects except those that are shared only within
    // thumbnails.

    // Part 9 contains all remaining objects.

    // We sort objects into the following categories:

    //   * open_document: part 4

    //   * first_page_private: part 6

    //   * first_page_shared: part 6

    //   * other_page_private: part 7

    //   * other_page_shared: part 8

    //   * thumbnail_private: part 9

    //   * thumbnail_shared: part 9

    //   * other: part 9

    //   * outlines: part 6 or 9

    part4_.clear();
    part6_.clear();
    part7_.clear();
    part8_.clear();
    part9_.clear();
    c_linp_ = LinParameters();
    c_page_offset_data_ = CHPageOffset();
    c_shared_object_data_ = CHSharedObject();
    c_outline_data_ = HGeneric();

    QPDFObjectHandle root = qpdf.getRoot();
    bool outlines_in_first_page = false;
    QPDFObjectHandle pagemode = root.getKey("/PageMode");
    QTC::TC("qpdf", "QPDF categorize pagemode present", pagemode.isName() ? 1 : 0);
    if (pagemode.isName()) {
        if (pagemode.getName() == "/UseOutlines") {
            if (root.hasKey("/Outlines")) {
                outlines_in_first_page = true;
            } else {
                QTC::TC("qpdf", "QPDF UseOutlines but no Outlines");
            }
        }
        QTC::TC("qpdf", "QPDF categorize pagemode outlines", outlines_in_first_page ? 1 : 0);
    }

    std::set<std::string> open_document_keys;
    open_document_keys.insert("/ViewerPreferences");
    open_document_keys.insert("/PageMode");
    open_document_keys.insert("/Threads");
    open_document_keys.insert("/OpenAction");
    open_document_keys.insert("/AcroForm");

    std::set<QPDFObjGen> lc_open_document;
    std::set<QPDFObjGen> lc_first_page_private;
    std::set<QPDFObjGen> lc_first_page_shared;
    std::set<QPDFObjGen> lc_other_page_private;
    std::set<QPDFObjGen> lc_other_page_shared;
    std::set<QPDFObjGen> lc_thumbnail_private;
    std::set<QPDFObjGen> lc_thumbnail_shared;
    std::set<QPDFObjGen> lc_other;
    std::set<QPDFObjGen> lc_outlines;
    std::set<QPDFObjGen> lc_root;

    for (auto& [og, ous]: object_to_obj_users_) {
        bool in_open_document = false;
        bool in_first_page = false;
        int other_pages = 0;
        int thumbs = 0;
        int others = 0;
        bool in_outlines = false;
        bool is_root = false;

        for (auto const& ou: ous) {
            switch (ou.ou_type) {
            case ObjUser::ou_trailer_key:
                if (ou.key == "/Encrypt") {
                    in_open_document = true;
                } else {
                    ++others;
                }
                break;

            case ObjUser::ou_thumb:
                ++thumbs;
                break;

            case ObjUser::ou_root_key:
                if (open_document_keys.contains(ou.key)) {
                    in_open_document = true;
                } else if (ou.key == "/Outlines") {
                    in_outlines = true;
                } else {
                    ++others;
                }
                break;

            case ObjUser::ou_page:
                if (ou.pageno == 0) {
                    in_first_page = true;
                } else {
                    ++other_pages;
                }
                break;

            case ObjUser::ou_root:
                is_root = true;
                break;
            }
        }

        if (is_root) {
            lc_root.insert(og);
        } else if (in_outlines) {
            lc_outlines.insert(og);
        } else if (in_open_document) {
            lc_open_document.insert(og);
        } else if ((in_first_page) && (others == 0) && (other_pages == 0) && (thumbs == 0)) {
            lc_first_page_private.insert(og);
        } else if (in_first_page) {
            lc_first_page_shared.insert(og);
        } else if ((other_pages == 1) && (others == 0) && (thumbs == 0)) {
            lc_other_page_private.insert(og);
        } else if (other_pages > 1) {
            lc_other_page_shared.insert(og);
        } else if ((thumbs == 1) && (others == 0)) {
            lc_thumbnail_private.insert(og);
        } else if (thumbs > 1) {
            lc_thumbnail_shared.insert(og);
        } else {
            lc_other.insert(og);
        }
    }

    // Generate ordering for objects in the output file.  Sometimes we just dump right from a set
    // into a vector.  Rather than optimizing this by going straight into the vector, we'll leave
    // these phases separate for now.  That way, this section can be concerned only with ordering,
    // and the above section can be considered only with categorization.  Note that sets of
    // QPDFObjGens are sorted by QPDFObjGen.  In a linearized file, objects appear in sequence with
    // the possible exception of hints tables which we won't see here anyway.  That means that
    // running calculateLinearizationData() on a linearized file should give results identical to
    // the original file ordering.

    // We seem to traverse the page tree a lot in this code, but we can address this for a future
    // code optimization if necessary. Premature optimization is the root of all evil.
    std::vector<QPDFObjectHandle> uc_pages;
    { // local scope
        // Map all page objects to the containing object stream.  This should be a no-op in a
        // properly linearized file.
        for (auto oh: pages) {
            uc_pages.emplace_back(getUncompressedObject(oh, object_stream_data));
        }
    }
    size_t npages = pages.size();

    // We will be initializing some values of the computed hint tables.  Specifically, we can
    // initialize any items that deal with object numbers or counts but not any items that deal with
    // lengths or offsets.  The code that writes linearized files will have to fill in these values
    // during the first pass.  The validation code can compute them relatively easily given the rest
    // of the information.

    // npages is the size of the existing pages vector, which has been created by traversing the
    // pages tree, and as such is a reasonable size.
    c_linp_.npages = npages;
    c_page_offset_data_.entries = std::vector<CHPageOffsetEntry>(npages);

    // Part 4: open document objects.  We don't care about the order.

    no_ci_stop_if(
        lc_root.size() != 1, "found other than one root while calculating linearization data" //
    );

    part4_.emplace_back(qpdf.getObject(*(lc_root.begin())));
    for (auto const& og: lc_open_document) {
        part4_.emplace_back(qpdf.getObject(og));
    }

    // Part 6: first page objects.  Note: implementation note 124 states that Acrobat always treats
    // page 0 as the first page for linearization regardless of /OpenAction.  pdlin doesn't provide
    // any option to set this and also disregards /OpenAction.  We will do the same.

    // First, place the actual first page object itself.
    no_ci_stop_if(
        pages.empty(), "no pages found while calculating linearization data" //
    );
    QPDFObjGen first_page_og(uc_pages.at(0).getObjGen());
    no_ci_stop_if(
        !lc_first_page_private.erase(first_page_og), "unable to linearize first page" //
    );
    c_linp_.first_page_object = uc_pages.at(0).getObjectID();
    part6_.emplace_back(uc_pages.at(0));

    // The PDF spec "recommends" an order for the rest of the objects, but we are going to disregard
    // it except to the extent that it groups private and shared objects contiguously for the sake
    // of hint tables.

    for (auto const& og: lc_first_page_private) {
        part6_.emplace_back(qpdf.getObject(og));
    }

    for (auto const& og: lc_first_page_shared) {
        part6_.emplace_back(qpdf.getObject(og));
    }

    // Place the outline dictionary if it goes in the first page section.
    if (outlines_in_first_page) {
        pushOutlinesToPart(part6_, lc_outlines, object_stream_data);
    }

    // Fill in page offset hint table information for the first page. The PDF spec says that
    // nshared_objects should be zero for the first page.  pdlin does not appear to obey this, but
    // it fills in garbage values for all the shared object identifiers on the first page.

    c_page_offset_data_.entries.at(0).nobjects = toI(part6_.size());

    // Part 7: other pages' private objects

    // For each page in order:
    for (size_t i = 1; i < npages; ++i) {
        // Place this page's page object

        QPDFObjGen page_og(uc_pages.at(i).getObjGen());
        no_ci_stop_if(
            !lc_other_page_private.erase(page_og),
            "unable to linearize page " + std::to_string(i) //
        );

        part7_.emplace_back(uc_pages.at(i));

        // Place all non-shared objects referenced by this page, updating the page object count for
        // the hint table.

        c_page_offset_data_.entries.at(i).nobjects = 1;

        ObjUser ou(ObjUser::ou_page, i);
        no_ci_stop_if(
            !obj_user_to_objects_.contains(ou),
            "found unreferenced page while calculating linearization data" //
        );

        for (auto const& og: obj_user_to_objects_[ou]) {
            if (lc_other_page_private.erase(og)) {
                part7_.emplace_back(qpdf.getObject(og));
                ++c_page_offset_data_.entries.at(i).nobjects;
            }
        }
    }
    // That should have covered all part7 objects.
    util::assertion(
        lc_other_page_private.empty(),
        "INTERNAL ERROR: QPDF::calculateLinearizationData: lc_other_page_private is not empty "
        "after generation of part7" //
    );

    // Part 8: other pages' shared objects

    // Order is unimportant.
    for (auto const& og: lc_other_page_shared) {
        part8_.emplace_back(qpdf.getObject(og));
    }

    // Part 9: other objects

    // The PDF specification makes recommendations on ordering here. We follow them only to a
    // limited extent.  Specifically, we put the pages tree first, then private thumbnail objects in
    // page order, then shared thumbnail objects, and then outlines (unless in part 6).  After that,
    // we throw all remaining objects in arbitrary order.

    // Place the pages tree.
    auto& pages_ogs = obj_user_to_objects_[{ObjUser::ou_root_key, "/Pages"}];
    no_ci_stop_if(
        pages_ogs.empty(), "found empty pages tree while calculating linearization data" //
    );
    for (auto const& og: pages_ogs) {
        if (lc_other.erase(og)) {
            part9_.emplace_back(qpdf.getObject(og));
        }
    }

    // Place private thumbnail images in page order.  Slightly more information would be required if
    // we were going to bother with thumbnail hint tables.
    for (size_t i = 0; i < npages; ++i) {
        QPDFObjectHandle thumb = uc_pages.at(i).getKey("/Thumb");
        thumb = getUncompressedObject(thumb, object_stream_data);
        QPDFObjGen thumb_og(thumb.getObjGen());
        // Output the thumbnail itself
        if (lc_thumbnail_private.erase(thumb_og) && !thumb.null()) {
            part9_.emplace_back(thumb);
        } else {
            // No internal error this time...there's nothing to stop this object from having
            // been referred to somewhere else outside of a page's /Thumb, and if it had been,
            // there's nothing to prevent it from having been in some set other than
            // lc_thumbnail_private.
        }
        for (auto const& og: obj_user_to_objects_[{ObjUser::ou_thumb, i}]) {
            if (lc_thumbnail_private.erase(og)) {
                part9_.emplace_back(qpdf.getObject(og));
            }
        }
    }
    util::assertion(
        lc_thumbnail_private.empty(),
        "INTERNAL ERROR: QPDF::calculateLinearizationData: lc_thumbnail_private not "
        "empty after placing thumbnails" //
    );

    // Place shared thumbnail objects
    for (auto const& og: lc_thumbnail_shared) {
        part9_.emplace_back(qpdf.getObject(og));
    }

    // Place outlines unless in first page
    if (!outlines_in_first_page) {
        pushOutlinesToPart(part9_, lc_outlines, object_stream_data);
    }

    // Place all remaining objects
    for (auto const& og: lc_other) {
        part9_.emplace_back(qpdf.getObject(og));
    }

    // Make sure we got everything exactly once.

    size_t num_placed =
        part4_.size() + part6_.size() + part7_.size() + part8_.size() + part9_.size();
    size_t num_wanted = object_to_obj_users_.size();
    no_ci_stop_if(
        // This can happen with damaged files, e.g. if the root is part of the the pages tree.
        num_placed != num_wanted,
        "QPDF::calculateLinearizationData: wrong number of objects placed (num_placed = " +
            std::to_string(num_placed) + "; number of objects: " + std::to_string(num_wanted) +
            "\nIf the file did not generate any other warnings please report this as a bug." //
    );

    // Calculate shared object hint table information including references to shared objects from
    // page offset hint data.

    // The shared object hint table consists of all part 6 (whether shared or not) in order followed
    // by all part 8 objects in order.  Add the objects to shared object data keeping a map of
    // object number to index.  Then populate the shared object information for the pages.

    // Note that two objects never have the same object number, so we can map from object number
    // only without regards to generation.
    std::map<int, int> obj_to_index;

    c_shared_object_data_.nshared_first_page = toI(part6_.size());
    c_shared_object_data_.nshared_total =
        c_shared_object_data_.nshared_first_page + toI(part8_.size());

    std::vector<CHSharedObjectEntry>& shared = c_shared_object_data_.entries;
    for (auto& oh: part6_) {
        int obj = oh.getObjectID();
        obj_to_index[obj] = toI(shared.size());
        shared.emplace_back(obj);
    }
    QTC::TC("qpdf", "QPDF lin part 8 empty", part8_.empty() ? 1 : 0);
    if (!part8_.empty()) {
        c_shared_object_data_.first_shared_obj = part8_.at(0).getObjectID();
        for (auto& oh: part8_) {
            int obj = oh.getObjectID();
            obj_to_index[obj] = toI(shared.size());
            shared.emplace_back(obj);
        }
    }
    no_ci_stop_if(
        std::cmp_not_equal(
            c_shared_object_data_.nshared_total, c_shared_object_data_.entries.size()),
        "shared object hint table has wrong number of entries" //
    );

    // Now compute the list of shared objects for each page after the first page.

    for (size_t i = 1; i < npages; ++i) {
        CHPageOffsetEntry& pe = c_page_offset_data_.entries.at(i);
        ObjUser ou(ObjUser::ou_page, i);
        no_ci_stop_if(
            !obj_user_to_objects_.contains(ou),
            "found unreferenced page while calculating linearization data" //
        );

        for (auto const& og: obj_user_to_objects_[ou]) {
            if (object_to_obj_users_[og].size() > 1 && obj_to_index.contains(og.getObj())) {
                int idx = obj_to_index[og.getObj()];
                ++pe.nshared_objects;
                pe.shared_identifiers.push_back(idx);
            }
        }
    }
}

template <typename T>
void
Lin::pushOutlinesToPart(
    std::vector<QPDFObjectHandle>& part,
    std::set<QPDFObjGen>& lc_outlines,
    T const& object_stream_data)
{
    QPDFObjectHandle root = qpdf.getRoot();
    QPDFObjectHandle outlines = root.getKey("/Outlines");
    if (outlines.null()) {
        return;
    }
    outlines = getUncompressedObject(outlines, object_stream_data);
    QPDFObjGen outlines_og(outlines.getObjGen());
    QTC::TC(
        "qpdf",
        "QPDF lin outlines in part",
        &part == &part6_         ? 0
            : (&part == &part9_) ? 1
                                 : 9999); // can't happen
    if (lc_outlines.erase(outlines_og)) {
        // Make sure outlines is in lc_outlines in case the file is damaged. in which case it may be
        // included in an earlier part.
        part.emplace_back(outlines);
        c_outline_data_.first_object = outlines_og.getObj();
        c_outline_data_.nobjects = 1;
    }
    for (auto const& og: lc_outlines) {
        if (!c_outline_data_.first_object) {
            c_outline_data_.first_object = og.getObj();
        }
        part.emplace_back(qpdf.getObject(og));
        ++c_outline_data_.nobjects;
    }
}

void
Lin::parts(
    QPDFWriter::ObjTable const& obj,
    std::vector<QPDFObjectHandle>& part4,
    std::vector<QPDFObjectHandle>& part6,
    std::vector<QPDFObjectHandle>& part7,
    std::vector<QPDFObjectHandle>& part8,
    std::vector<QPDFObjectHandle>& part9)
{
    calculateLinearizationData(obj);
    part4 = part4_;
    part6 = part6_;
    part7 = part7_;
    part8 = part8_;
    part9 = part9_;
}

static inline int
nbits(int val)
{
    return (val == 0 ? 0 : (1 + nbits(val >> 1)));
}

int
Lin::outputLengthNextN(
    int in_object, int n, QPDFWriter::NewObjTable const& new_obj, QPDFWriter::ObjTable const& obj)
{
    // Figure out the length of a series of n consecutive objects in the output file starting with
    // whatever object in_object from the input file mapped to.

    int first = obj[in_object].renumber;
    int last = first + n;
    no_ci_stop_if(
        first <= 0, "found object that is not renumbered while writing linearization data");
    qpdf_offset_t length = 0;
    for (int i = first; i < last; ++i) {
        auto l = new_obj[i].length;
        no_ci_stop_if(
            l == 0, "found item with unknown length while writing linearization data" //
        );
        length += l;
    }
    return toI(length);
}

void
Lin::calculateHPageOffset(QPDFWriter::NewObjTable const& new_obj, QPDFWriter::ObjTable const& obj)
{
    // Page Offset Hint Table

    // We are purposely leaving some values set to their initial zero values.

    auto const& all_pages = pages.all();
    size_t npages = all_pages.size();
    CHPageOffset& cph = c_page_offset_data_;
    std::vector<CHPageOffsetEntry>& cphe = cph.entries;

    // Calculate minimum and maximum values for number of objects per page and page length.

    int min_nobjects = std::numeric_limits<int>::max();
    int max_nobjects = 0;
    int min_length = std::numeric_limits<int>::max();
    int max_length = 0;
    int max_shared = 0;

    HPageOffset& ph = page_offset_hints_;
    std::vector<HPageOffsetEntry>& phe = ph.entries;
    // npages is the size of the existing pages array.
    phe = std::vector<HPageOffsetEntry>(npages);

    size_t i = 0;
    for (auto& phe_i: phe) {
        // Calculate values for each page, assigning full values to the delta items.  They will be
        // adjusted later.

        // Repeat calculations for page 0 so we can assign to phe[i] without duplicating those
        // assignments.

        int nobjects = cphe.at(i).nobjects;
        int length = outputLengthNextN(all_pages.at(i).getObjectID(), nobjects, new_obj, obj);
        int nshared = cphe.at(i).nshared_objects;

        min_nobjects = std::min(min_nobjects, nobjects);
        max_nobjects = std::max(max_nobjects, nobjects);
        min_length = std::min(min_length, length);
        max_length = std::max(max_length, length);
        max_shared = std::max(max_shared, nshared);

        phe_i.delta_nobjects = nobjects;
        phe_i.delta_page_length = length;
        phe_i.nshared_objects = nshared;
        ++i;
    }

    ph.min_nobjects = min_nobjects;
    ph.first_page_offset = new_obj[obj[all_pages.at(0)].renumber].xref.getOffset();
    ph.nbits_delta_nobjects = nbits(max_nobjects - min_nobjects);
    ph.min_page_length = min_length;
    ph.nbits_delta_page_length = nbits(max_length - min_length);
    ph.nbits_nshared_objects = nbits(max_shared);
    ph.nbits_shared_identifier = nbits(c_shared_object_data_.nshared_total);
    ph.shared_denominator = 4; // doesn't matter

    // It isn't clear how to compute content offset and content length.  Since we are not
    // interleaving page objects with the content stream, we'll use the same values for content
    // length as page length.  We will use 0 as content offset because this is what Adobe does
    // (implementation note 127) and pdlin as well.
    ph.nbits_delta_content_length = ph.nbits_delta_page_length;
    ph.min_content_length = ph.min_page_length;

    i = 0;
    for (auto& phe_i: phe) {
        // Adjust delta entries
        if (phe_i.delta_nobjects < min_nobjects || phe_i.delta_page_length < min_length) {
            stopOnError(
                "found too small delta nobjects or delta page length while writing "
                "linearization data");
        }
        phe_i.delta_nobjects -= min_nobjects;
        phe_i.delta_page_length -= min_length;
        phe_i.delta_content_length = phe_i.delta_page_length;

        auto& si = cphe.at(i).shared_identifiers;
        phe_i.shared_identifiers.insert(phe_i.shared_identifiers.end(), si.begin(), si.end());
        phe_i.shared_numerators.insert(phe_i.shared_numerators.end(), si.size(), 0);
        ++i;
    }
}

void
Lin::calculateHSharedObject(QPDFWriter::NewObjTable const& new_obj, QPDFWriter::ObjTable const& obj)
{
    CHSharedObject& cso = c_shared_object_data_;
    std::vector<CHSharedObjectEntry>& csoe = cso.entries;
    HSharedObject& so = shared_object_hints_;
    std::vector<HSharedObjectEntry>& soe = so.entries;
    soe.clear();

    int min_length = outputLengthNextN(csoe.at(0).object, 1, new_obj, obj);
    int max_length = min_length;

    for (size_t i = 0; i < toS(cso.nshared_total); ++i) {
        // Assign absolute numbers to deltas; adjust later
        int length = outputLengthNextN(csoe.at(i).object, 1, new_obj, obj);
        min_length = std::min(min_length, length);
        max_length = std::max(max_length, length);
        soe.emplace_back();
        soe.at(i).delta_group_length = length;
    }
    no_ci_stop_if(
        soe.size() != toS(cso.nshared_total), "soe has wrong size after initialization" //
    );

    so.nshared_total = cso.nshared_total;
    so.nshared_first_page = cso.nshared_first_page;
    if (so.nshared_total > so.nshared_first_page) {
        so.first_shared_obj = obj[cso.first_shared_obj].renumber;
        so.min_group_length = min_length;
        so.first_shared_offset = new_obj[so.first_shared_obj].xref.getOffset();
    }
    so.min_group_length = min_length;
    so.nbits_delta_group_length = nbits(max_length - min_length);

    for (size_t i = 0; i < toS(cso.nshared_total); ++i) {
        // Adjust deltas
        no_ci_stop_if(
            soe.at(i).delta_group_length < min_length,
            "found too small group length while writing linearization data" //
        );

        soe.at(i).delta_group_length -= min_length;
    }
}

void
Lin::calculateHOutline(QPDFWriter::NewObjTable const& new_obj, QPDFWriter::ObjTable const& obj)
{
    HGeneric& cho = c_outline_data_;

    if (cho.nobjects == 0) {
        return;
    }

    HGeneric& ho = outline_hints_;

    ho.first_object = obj[cho.first_object].renumber;
    ho.first_object_offset = new_obj[ho.first_object].xref.getOffset();
    ho.nobjects = cho.nobjects;
    ho.group_length = outputLengthNextN(cho.first_object, ho.nobjects, new_obj, obj);
}

template <class T, class int_type>
static void
write_vector_int(BitWriter& w, int nitems, std::vector<T>& vec, int bits, int_type T::* field)
{
    // nitems times, write bits bits from the given field of the ith vector to the given bit writer.

    for (size_t i = 0; i < QIntC::to_size(nitems); ++i) {
        w.writeBits(QIntC::to_ulonglong(vec.at(i).*field), QIntC::to_size(bits));
    }
    // The PDF spec says that each hint table starts at a byte boundary.  Each "row" actually must
    // start on a byte boundary.
    w.flush();
}

template <class T>
static void
write_vector_vector(
    BitWriter& w,
    int nitems1,
    std::vector<T>& vec1,
    int T::* nitems2,
    int bits,
    std::vector<int> T::* vec2)
{
    // nitems1 times, write nitems2 (from the ith element of vec1) items from the vec2 vector field
    // of the ith item of vec1.
    for (size_t i1 = 0; i1 < QIntC::to_size(nitems1); ++i1) {
        for (size_t i2 = 0; i2 < QIntC::to_size(vec1.at(i1).*nitems2); ++i2) {
            w.writeBits(QIntC::to_ulonglong((vec1.at(i1).*vec2).at(i2)), QIntC::to_size(bits));
        }
    }
    w.flush();
}

void
Lin::writeHPageOffset(BitWriter& w)
{
    HPageOffset& t = page_offset_hints_;

    w.writeBitsInt(t.min_nobjects, 32);               // 1
    w.writeBits(toULL(t.first_page_offset), 32);      // 2
    w.writeBitsInt(t.nbits_delta_nobjects, 16);       // 3
    w.writeBitsInt(t.min_page_length, 32);            // 4
    w.writeBitsInt(t.nbits_delta_page_length, 16);    // 5
    w.writeBits(toULL(t.min_content_offset), 32);     // 6
    w.writeBitsInt(t.nbits_delta_content_offset, 16); // 7
    w.writeBitsInt(t.min_content_length, 32);         // 8
    w.writeBitsInt(t.nbits_delta_content_length, 16); // 9
    w.writeBitsInt(t.nbits_nshared_objects, 16);      // 10
    w.writeBitsInt(t.nbits_shared_identifier, 16);    // 11
    w.writeBitsInt(t.nbits_shared_numerator, 16);     // 12
    w.writeBitsInt(t.shared_denominator, 16);         // 13

    int nitems = toI(pages.size());
    std::vector<HPageOffsetEntry>& entries = t.entries;

    write_vector_int(w, nitems, entries, t.nbits_delta_nobjects, &HPageOffsetEntry::delta_nobjects);
    write_vector_int(
        w, nitems, entries, t.nbits_delta_page_length, &HPageOffsetEntry::delta_page_length);
    write_vector_int(
        w, nitems, entries, t.nbits_nshared_objects, &HPageOffsetEntry::nshared_objects);
    write_vector_vector(
        w,
        nitems,
        entries,
        &HPageOffsetEntry::nshared_objects,
        t.nbits_shared_identifier,
        &HPageOffsetEntry::shared_identifiers);
    write_vector_vector(
        w,
        nitems,
        entries,
        &HPageOffsetEntry::nshared_objects,
        t.nbits_shared_numerator,
        &HPageOffsetEntry::shared_numerators);
    write_vector_int(
        w, nitems, entries, t.nbits_delta_content_offset, &HPageOffsetEntry::delta_content_offset);
    write_vector_int(
        w, nitems, entries, t.nbits_delta_content_length, &HPageOffsetEntry::delta_content_length);
}

void
Lin::writeHSharedObject(BitWriter& w)
{
    HSharedObject& t = shared_object_hints_;

    w.writeBitsInt(t.first_shared_obj, 32);         // 1
    w.writeBits(toULL(t.first_shared_offset), 32);  // 2
    w.writeBitsInt(t.nshared_first_page, 32);       // 3
    w.writeBitsInt(t.nshared_total, 32);            // 4
    w.writeBitsInt(t.nbits_nobjects, 16);           // 5
    w.writeBitsInt(t.min_group_length, 32);         // 6
    w.writeBitsInt(t.nbits_delta_group_length, 16); // 7

    QTC::TC(
        "qpdf",
        "QPDF lin write nshared_total > nshared_first_page",
        (t.nshared_total > t.nshared_first_page) ? 1 : 0);

    int nitems = t.nshared_total;
    std::vector<HSharedObjectEntry>& entries = t.entries;

    write_vector_int(
        w, nitems, entries, t.nbits_delta_group_length, &HSharedObjectEntry::delta_group_length);
    write_vector_int(w, nitems, entries, 1, &HSharedObjectEntry::signature_present);
    for (size_t i = 0; i < toS(nitems); ++i) {
        // If signature were present, we'd have to write a 128-bit hash.
        if (entries.at(i).signature_present != 0) {
            stopOnError("found unexpected signature present while writing linearization data");
        }
    }
    write_vector_int(w, nitems, entries, t.nbits_nobjects, &HSharedObjectEntry::nobjects_minus_one);
}

void
Lin::writeHGeneric(BitWriter& w, HGeneric& t)
{
    w.writeBitsInt(t.first_object, 32);            // 1
    w.writeBits(toULL(t.first_object_offset), 32); // 2
    w.writeBitsInt(t.nobjects, 32);                // 3
    w.writeBitsInt(t.group_length, 32);            // 4
}

void
Lin::generateHintStream(
    QPDFWriter::NewObjTable const& new_obj,
    QPDFWriter::ObjTable const& obj,
    std::string& hint_buffer,
    int& S,
    int& O,
    bool compressed)
{
    // Populate actual hint table values
    calculateHPageOffset(new_obj, obj);
    calculateHSharedObject(new_obj, obj);
    calculateHOutline(new_obj, obj);

    // Write the hint stream itself into a compressed memory buffer. Write through a counter so we
    // can get offsets.
    pl::Count c(0, hint_buffer);
    BitWriter w(&c);

    writeHPageOffset(w);
    S = toI(c.getCount());
    writeHSharedObject(w);
    O = 0;
    if (outline_hints_.nobjects > 0) {
        O = toI(c.getCount());
        writeHGeneric(w, outline_hints_);
    }
    if (compressed) {
        hint_buffer = pl::pipe<Pl_Flate>(hint_buffer, Pl_Flate::a_deflate);
    }
}
