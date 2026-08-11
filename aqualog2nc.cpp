// aqualog2nc.cpp
//
// Exports Aqualog EEM/absorbance data from one .opj/.ogw file, or every
// .opj/.ogw file found under a folder, into a single NetCDF file -
// without needing Origin Pro installed.
//
// Usage: aqualog2nc <input.opj | input.ogw | input_folder> output.nc
//
// Only samples whose Experiment Type is exactly
// "3D Acquisition[EEM 3D CCD + Absorbance]" are exported (see
// kRequiredExperimentType below) - everything else is skipped.
//
// Every workbook in every input .opj/.ogw becomes its own top-level NetCDF
// group, each with its own independent emission/excitation axes AND its
// own XCorrect/MCorrect variables - there is no assumption that different
// samples share a common wavelength grid or correction factors. This
// means a folder can safely accumulate samples measured with different
// settings (even by accident) without corrupting each other.
//
// Dispatches on worksheet *type* instead of treating every sheet as a
// generic 2D matrix - mirrors the switch(sheetnames{k}) structure in
// aqualogimport.m. See the comments on each write* function for the
// specific MATLAB lines they correspond to.
//
// PADDED-ROW HANDLING
// Origin sometimes reserves more rows in a spreadsheet column than an
// acquisition actually used; the extra rows read back as sentinel values
// (e.g. -0.000) outside any real wavelength. validAscendingRowOrder() uses
// the instrument's physical wavelength range (200-850 nm - adjust
// kMinPhysicalWavelength / kMaxPhysicalWavelength below if yours differs)
// to find the real rows in a wavelength-axis column, and every reader in
// this file uses that same row index list when pulling aligned data from
// the *other* columns in that row, so nothing shifts out of alignment.
//
// EXCITATION / ABSORBANCE AXIS
// Under this instrument's combined acquisition protocol, the EEM
// excitation grid and the absorbance wavelength grid are always identical,
// so they share a single "excitation" axis per workbook (sourced from
// whichever of R1andR1cSample/Blank or AbsSpectrumSample/Blank is found
// first in that workbook).
//
// EEM MATRIX DIMENSION ORDER (MATLAB compatibility)
// netCDF stores dimensions in C order (first declared = slowest-varying),
// but MATLAB's ncread() reverses dimension order on read to keep indexing
// natural for its column-major arrays. writeEemMatrix() declares
// (excitation, emission) - the reverse of the natural reading order - so
// that ncread() in MATLAB returns an array shaped (emission, excitation),
// matching how the Aqualog software displays it. The values array is
// filled to match: excitation-major (outer), emission-minor (inner).
//
// XCORRECT CONSOLIDATION
// Up to three sheets can each carry a copy of the excitation correction
// factor (R1andR1cBlank, R1andR1cSample, AbsSpectrumBlank) - these should
// all represent the same physical quantity. writeXCorrect() gathers
// whichever of these exist for a workbook, compares them, and writes a
// single "XCorrect" variable (preferring R1andR1cBlank as the canonical
// source, matching aqualogimport.m's Xout.XCorrect). MCorrect has only
// ever had one source, so it's unchanged, still written per-workbook on
// the emission axis.
//
// REDUNDANT ABSORBANCE FIELDS DROPPED
// AbsSpectrumSample/Blank's R1, R1dark, and "horiba backup" columns were
// confirmed by manual cross-check against the Aqualog software to be
// identical to R1_Sample/R1_Blank/R1dark_Sample/R1dark_Blank (already
// written from the R1andR1c sheets), so writeAbsSpectrum() now only
// writes AbsI1_Sample/Blank and AbsI1dark_Sample/Blank - the values that
// are unique to the absorbance sheet.
//
// SAMPLE TIMESTAMPS
// Origin::Excel inherits Window::creationDate / Window::modificationDate
// (time_t), which Origin sets when the workbook is created/last modified.
// These are written per-workbook as "creation_time" / "modification_time"
// ISO-8601 UTC string attributes.
//
// SAMPLE NAMING
// Origin::Window::name is the short internal identifier (e.g. "Book1") -
// not what Origin displays, and not what aqualogimport.m uses.
// Origin::Window::label is the long, human-readable name (with Origin's
// own "(01)" auto-numbering for reused base names), matching MATLAB's
// LongName. Aqualog's naming template appends a fixed, non-sample-specific
// descriptor after the identifier (e.g. "AO22020 (01) - 3D Acquisition EEM
// 3D CCD - Absorbance"); only the part through the closing parenthesis is
// used for the group name - the full label is still kept untouched in the
// workbook_name attribute.
//
// PRE-EXPORT CONSISTENCY VALIDATION
// Before anything is written for a workbook, validateWorkbookAxes() checks
// that every axis-bearing sheet agrees on the emission/excitation counts:
//   0) an S1Sample sheet must exist at all - its absence means this
//      workbook is a blank measurement, not a sample, and is skipped with
//      its own distinct diagnostic message (isBlank on the result)
//   1) S1Sample row count               == emission axis length
//   2) S1Sample/S1Blank column count    == excitation axis length
//   3) AbsSpectrumSample/Blank row count == excitation axis length
//   4) R1andR1cSample/Blank row count    == excitation axis length
//   5) S1DarkandMCorrectSample/Blank row count == emission axis length
// (excitation vs. XCorrect is covered by checks 3/4, since XCorrect is
// sourced from those same sheets). If any sheet disagrees - typically an
// acquisition that was aborted partway through, leaving some sheets
// truncated relative to others - the whole sample is skipped before any
// NetCDF group is created, rather than exported with silent NaN gaps.
//
// BATCH MODE / GROUP NAME COLLISIONS
// Two different .opj files can each contain a workbook with the same name
// (e.g. both call it "Sample_1"). uniqueGroupName() appends a numeric
// suffix in that case, and every group also gets a source_opj_file
// attribute so you can always trace a sample back to its origin file.
// Collision tracking uses an in-memory set of names already used, rather
// than repeatedly querying the NetCDF file's existing groups.
//
// PERFORMANCE OVER LARGE BATCHES (hundreds of samples)
// nc.set_Fill(NC_NOFILL, ...) disables netCDF's default pre-fill-then-
// overwrite behavior for new variables. Since every variable here is
// always fully populated immediately after creation, that pre-fill write
// is pure redundant I/O in this code's usage pattern. Per-sample elapsed-
// time logging (via <chrono>) is also printed to stdout for visibility
// into large-batch runs.
//
// ASSUMPTIONS TO VERIFY AGAINST YOUR OriginObj.h / OriginFile.h:
//   - Origin::Variant has type() returning V_DOUBLE or V_STRING, plus
//     as_double() / as_string(). Origin::Window has time_t creationDate,
//     modificationDate, and string name/label. (Confirmed against the
//     public liborigin fork's OriginObj.h - your local copy may differ
//     slightly.)
//   - Origin::SpreadSheet::columns[i].data is a vector<Origin::Variant>.
//   - Worksheet short names may contain spaces/separators the way Origin
//     displays them (e.g. "S1 Sample"); normalizeSheetName() strips the
//     same characters aqualogimport.m does before comparing.
//   - S1DarkandMCorrectSample/Blank's column 0 is an emission wavelength
//     axis, following the same column-0-is-the-axis convention as every
//     other sheet type here - inferred from the established pattern, not
//     independently confirmed against this specific sheet.
//   - Different sheets in the same workbook share the same raw row
//     layout/padding, so row indices computed from one sheet's axis
//     column are valid to reuse when reading another sheet's data
//     columns (including the different XCorrect source sheets). This
//     should hold for a single combined acquisition run; the diagnostic
//     printouts below will make it obvious if it doesn't.

#include "OriginFile.h"

#include <netcdf>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

using namespace netCDF;
using namespace netCDF::exceptions;
namespace fs = std::filesystem;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// Physical wavelength envelope of the instrument. Any row whose axis value
// falls outside this range is treated as unused/padded and dropped.
constexpr double kMinPhysicalWavelength = 200.0;
constexpr double kMaxPhysicalWavelength = 850.0;

// How far apart two XCorrect sources are allowed to be before we warn
// about a real disagreement rather than floating-point noise.
constexpr double kXCorrectTolerance = 1e-6;

// Only samples whose <ExpType> (see extractExpSummaryFields() below) equals
// this exactly are exported; everything else - including samples where
// <ExpType> is missing entirely - is skipped. Adjust if you need to export
// a different Aqualog acquisition mode.
const std::string kRequiredExperimentType = "3D Acquisition[EEM 3D CCD + Absorbance]";

// ---------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------

// Keeps only letters, digits, and underscores (dropping parentheses
// entirely rather than substituting them, e.g. "(01)" -> "01"), collapses
// repeated underscores, and trims them from both ends. Used for NetCDF
// group/variable names, which are far stricter about allowed characters
// than Origin's own sample-naming conventions.
std::string safeName(const std::string& input)
{
    std::string out;
    out.reserve(input.size());
    for (char c : input)
    {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
            out += c;
        else if (c == '(' || c == ')')
            continue;
        else
            out += '_';
    }

    std::string collapsed;
    collapsed.reserve(out.size());
    for (char c : out)
    {
        if (c == '_' && !collapsed.empty() && collapsed.back() == '_')
            continue;
        collapsed += c;
    }
    while (!collapsed.empty() && collapsed.front() == '_')
        collapsed.erase(collapsed.begin());
    while (!collapsed.empty() && collapsed.back() == '_')
        collapsed.pop_back();
    out = collapsed;

    if (out.empty())
        out = "unnamed";
    if (std::isdigit(static_cast<unsigned char>(out[0])))
        out = "_" + out;

    return out;
}

// Mirrors erase(worksheetName,{' ','/','-',':','_'}) in aqualogimport.m -
// used to match sheet short names against known keys like "S1Sample".
std::string normalizeSheetName(const std::string& input)
{
    std::string out;
    out.reserve(input.size());
    for (char c : input)
    {
        if (c != ' ' && c != '/' && c != '-' && c != ':' && c != '_')
            out += c;
    }
    return out;
}

double variantToDouble(const Origin::Variant& v)
{
    if (v.type() == Origin::Variant::V_DOUBLE)
        return v.as_double();
    return kNaN;
}

bool contains(const std::string& s, const std::string& k)
{
    return s.find(k) != std::string::npos;
}

// Every Aqualog workbook template includes a page literally named "Note"
// (see the LTCONST tree in the workbook's raw property block, which lists
// it alongside S1Sample/AbsSpectrumBlank/etc. as just another numbered
// page). It is genuinely empty as *stored spreadsheet cell data* on every
// workbook checked - Aqualog's own "[EXP_FD_FILE] / EXPERIMENT SUMMARY:"
// note text is not saved there verbatim. Kept as a fallback in case a
// given file DOES have something an operator actually typed on this page
// (it would come back as a V_STRING cell); concatenates every non-empty
// string cell found on the sheet, in column-then-row order.
// Extracts the text between <tag>...</tag>, or "" if not found.
std::string extractXmlTag(const std::string& text, const std::string& tag)
{
    std::string openTag = "<" + tag + ">";
    std::string closeTag = "</" + tag + ">";
    size_t start = text.find(openTag);
    if (start == std::string::npos)
        return "";
    start += openTag.size();
    size_t end = text.find(closeTag, start);
    if (end == std::string::npos)
        return "";
    return text.substr(start, end - start);
}

// Aqualog's GENERAL PARAMETERS report gives each sample a "Data
// Identifier" (e.g. "quninesulfate", "Dana0013", "ppl00") that is NOT part
// of the <ExpSummary> XML - it comes from the workbook's own long
// name/label instead. The label's first line is consistently
// "<DataIdentifier> (<NN>)" followed by a newline and the experiment type/
// comment text; the "(NN)" is some per-workbook creation-order counter, not
// part of the identifier. Confirmed against all 5 independently-provided
// real-world files (dana12/fs17/qsbs/trm_01/trm_02, spanning 3 different
// physical instruments): the extracted value matched each file's
// ground-truth "Data Identifier:" exactly (Dana0013, MOS17111,
// quninesulfate, ppl00, ppl02).
std::string extractDataIdentifier(const std::string& label)
{
    size_t newline = label.find('\n');
    std::string firstLine = (newline == std::string::npos) ? label : label.substr(0, newline);
    while (!firstLine.empty() && (firstLine.back() == '\r' || firstLine.back() == ' '))
        firstLine.pop_back(); // labels use CRLF line endings

    size_t openParen = firstLine.rfind(" (");
    if (openParen != std::string::npos && !firstLine.empty() && firstLine.back() == ')')
    {
        bool digitsOnly = !firstLine.empty();
        for (size_t i = openParen + 2; i + 1 < firstLine.size(); i++)
        {
            if (!isdigit(static_cast<unsigned char>(firstLine[i])))
            {
                digitsOnly = false;
                break;
            }
        }
        if (digitsOnly)
            firstLine = firstLine.substr(0, openParen);
    }
    return firstLine;
}

// Aqualog stores a small per-workbook <ExpSummary> XML block (inside
// Window::rawPropertyBlock, which liborigin exposes as the raw tail of the
// window's property header) with <ExpType>, <IntegrationTime>,
// <IntegrationTimeUnits>, and <ExpFilename> tags. Confirmed against test.opj
// and all 5 testdata files: <ExpType> reproduces Aqualog's "Experiment
// Type:" value exactly, and <ExpFilename> reproduces "Source Acquisition
// File:". These two fields below are pulled directly from that XML.
//
// <IntegrationTime> is NOT used for the integration_time attribute despite
// also being available here - it stores the raw double with binary
// floating-point representation error visible (e.g. "0.050000" comes back
// as "5.0000000000000003e-002"), whereas the compressed report's own
// "Integration Time: 0.050000" line is Aqualog's own pre-formatted, clean
// decimal string. See extractIntegrationTimeText() below, which pulls it
// from there instead.
//
// UPDATE - the rest of the report (Park:, Grating:, Detector settings,
// CFG_NAME=, EX1=/EM1=/S1=/A1=/R1= device assignments, the embedded
// [EXP_FILE] acquisition XML) turned out to BE persisted after all - not
// as literal ASCII/UTF-16LE/plain-float bytes (hence the earlier exhaustive
// search below finding nothing), but compressed with PKWare Data
// Compression Library's "Implode" scheme. See the "Compressed Note text"
// section below (decodeCompressedNotes() / pkware::decode()) for the
// decoder - confirmed byte-exact against a live capture of Origin's own
// decompressor (see the runbook) - Integration Time, Park wavelength, and
// the CCD settings are all pulled from it there. CFG_NAME is still not
// recovered here; it turned out to be a static per-instrument model
// identifier defined in the bundled Aqualog software's own jySystems.xml,
// not saved per-project at all, so there's nothing to decode for that one
// specifically.
//
// (Original exhaustive-search note, kept for context: checked against all
// 5 real-world testdata files, 3 different physical instruments, via
// literal ASCII search, UTF-16LE search, exact IEEE-754 double search, and
// exact float32 search for each file's own ground-truth Park value and
// CFG_NAME string - zero hits in every case, which is what motivated
// looking at the compressed embedded-page storage in the first place.)
struct ExpSummaryFields
{
    std::string expType;
    std::string expFilename;
};

ExpSummaryFields extractExpSummaryFields(const std::string& rawPropertyBlock)
{
    ExpSummaryFields fields;
    fields.expType = extractXmlTag(rawPropertyBlock, "ExpType");
    fields.expFilename = extractXmlTag(rawPropertyBlock, "ExpFilename");
    return fields;
}

// ---------------------------------------------------------------------
// Compressed Note text (PKWare Data Compression Library "Implode")
// ---------------------------------------------------------------------
//
// Aqualog's full auto-generated "GENERAL PARAMETERS:" report - the part
// extractExpSummaryFields() above can't reach (Park:, Grating:, detector
// settings, the embedded [EXP_FILE] acquisition XML) - lives per-workbook
// in one of several generic "@${[0|<kind>|_Storage_Ebdded_pages_Data_|
// <size>|<checksum>]}" embedded-page storage records (the same mechanism
// Origin uses for graph thumbnails), compressed with PKWare's "Implode"
// scheme (kind 5; the format behind old ZIP method 6 / MPQ archives) once
// the note is long enough - which every auto-generated report is. See
// origin_note_decompression_runbook.md and note_decompression_research/ in
// this repo for the full reverse-engineering writeup.
//
// CONFIRMED CORRECT: both the decode() logic below and the escaping
// scheme in unescapeStorageBytes() were verified byte-for-byte against a
// live capture of Origin's own decompressor input/output (a Frida hook on
// OPack9.dll!opkUnCompressBufferToBuffer inside a real Origin process,
// documented in the runbook's "Session 4" and "Session 6") - all 5 known
// testdata files now decode the full report text ("GENERAL PARAMETERS:"
// through "ACCESSORIES:" and beyond) with zero byte errors. That said,
// this has only been checked against those 5 files; decode() below still
// stops early (returning whatever came out cleanly so far) if it ever
// hits a genuinely inconsistent bitstream on a file that turns out to
// exercise something these 5 didn't, so every caller of this decoded text
// should still treat a field as "present if found, silently absent
// otherwise" rather than assuming it's always there.
//
// A workbook's own "@${[...]}" record isn't at any fixed position - each
// workbook's template reserves 20 sample-note "slots" (confirmed: 20 such
// records per workbook in every file checked, almost all empty), so
// instead of guessing a slot, decodeCompressedNotes() decodes every kind-5
// record in the file and each one is matched back to a workbook by its own
// "Data Identifier: " line (which - being near the top of the report -
// decodes reliably even when Park and later fields don't), rather than
// assuming any particular file position.
namespace pkware
{

constexpr int kMaxBits = 13;

struct HuffmanTable
{
    std::vector<short> count;   // number of codes of each bit length
    std::vector<short> symbol;  // symbols in canonical order
};

// `rep` packs a run-length-encoded list of Huffman code lengths: each byte
// is (repeat_count - 1) in the high nibble, bit length in the low nibble.
// Mirrors construct() in Mark Adler's public-domain "blast.c" reference
// PKWare-Implode decompressor (zlib/libpng license; see
// note_decompression_research/blast/blast.c in this repo for the
// original), which this whole pkware:: namespace is adapted from.
HuffmanTable buildHuffmanTable(const unsigned char* rep, size_t n)
{
    std::vector<unsigned char> length;
    for (size_t i = 0; i < n; i++)
    {
        unsigned char len = rep[i] & 0x0F;
        unsigned char repeat = (rep[i] >> 4) + 1;
        for (unsigned char r = 0; r < repeat; r++)
            length.push_back(len);
    }

    HuffmanTable table;
    table.count.assign(kMaxBits + 1, 0);
    for (unsigned char len : length)
        table.count[len]++;

    std::vector<short> offs(kMaxBits + 2, 0);
    for (int len = 1; len <= kMaxBits; len++)
        offs[len + 1] = static_cast<short>(offs[len] + table.count[len]);

    table.symbol.assign(length.size(), 0);
    std::vector<short> next = offs;
    for (size_t sym = 0; sym < length.size(); sym++)
    {
        if (length[sym] != 0)
            table.symbol[next[length[sym]]++] = static_cast<short>(sym);
    }
    return table;
}

// PKWare Implode packs bits into bytes least-significant-bit first.
class BitReader
{
public:
    explicit BitReader(const std::string& data) : data_(data) {}

    int bit()  // -1 once input is exhausted
    {
        size_t byteIndex = pos_ / 8;
        if (byteIndex >= data_.size())
            return -1;
        int b = (static_cast<unsigned char>(data_[byteIndex]) >> (pos_ % 8)) & 1;
        pos_++;
        return b;
    }

    int bits(int need)  // -1 if it runs out of input partway through
    {
        int value = 0;
        for (int i = 0; i < need; i++)
        {
            int b = bit();
            if (b < 0)
                return -1;
            value |= b << i;
        }
        return value;
    }

private:
    const std::string& data_;
    size_t pos_ = 0;
};

// -1 on exhausted input or an incomplete/invalid code.
int decodeSymbol(BitReader& reader, const HuffmanTable& table)
{
    int code = 0, first = 0, index = 0, len = 1;
    while (len <= kMaxBits)
    {
        int b = reader.bit();
        if (b < 0)
            return -1;
        code |= (b ^ 1);
        int count = table.count[len];
        if (code < first + count)
            return table.symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
        len++;
    }
    return -1;
}

// Bit-length tables for the three canonical PKWare Implode Huffman trees
// (literals, match lengths, match distances) - fixed/static, not stored
// per file, which is why different files' compressed notes can share
// identical compressed byte runs at template-identical content.
const unsigned char kLiteralLengths[] = {
    11, 124, 8, 7, 28, 7, 188, 13, 76, 4, 10, 8, 12, 10, 12, 10, 8, 23, 8,
    9, 7, 6, 7, 8, 7, 6, 55, 8, 23, 24, 12, 11, 7, 9, 11, 12, 6, 7, 22, 5,
    7, 24, 6, 11, 9, 6, 7, 22, 7, 11, 38, 7, 9, 8, 25, 11, 8, 11, 9, 12,
    8, 12, 5, 38, 5, 38, 5, 11, 7, 5, 6, 21, 6, 10, 53, 8, 7, 24, 10, 27,
    44, 253, 253, 253, 252, 252, 252, 13, 12, 45, 12, 45, 12, 61, 12, 45,
    44, 173};
const unsigned char kLengthLengths[] = {2, 35, 36, 53, 38, 23};
const unsigned char kDistanceLengths[] = {2, 20, 53, 230, 247, 151, 248};
const short kLengthBase[16] = {3, 2, 4, 5, 6, 7, 8, 9, 10, 12, 16, 24, 40, 72, 136, 264};
const char kLengthExtraBits[16] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8};

// Decodes a PKWare Implode stream, stopping (and returning whatever came
// out so far) at the first sign of an inconsistent bitstream. See the big
// comment above this namespace for what "best-effort" means here.
std::string decode(const std::string& compressed)
{
    if (compressed.size() < 2)
        return "";

    BitReader reader(compressed);
    int literalsCoded = reader.bits(8);
    int dictBits = reader.bits(8);
    if (literalsCoded < 0 || literalsCoded > 1 || dictBits < 4 || dictBits > 6)
        return "";

    HuffmanTable litTable = buildHuffmanTable(kLiteralLengths, sizeof(kLiteralLengths));
    HuffmanTable lenTable = buildHuffmanTable(kLengthLengths, sizeof(kLengthLengths));
    HuffmanTable distTable = buildHuffmanTable(kDistanceLengths, sizeof(kDistanceLengths));

    std::string out;
    out.reserve(compressed.size() * 6);  // typical ratio seen on this data

    while (true)
    {
        int flag = reader.bit();
        if (flag < 0)
            break;

        if (flag == 0)
        {
            int symbol = literalsCoded ? decodeSymbol(reader, litTable) : reader.bits(8);
            if (symbol < 0)
                break;
            out += static_cast<char>(symbol);
            continue;
        }

        int lenSymbol = decodeSymbol(reader, lenTable);
        if (lenSymbol < 0)
            break;
        int extraLen = reader.bits(kLengthExtraBits[lenSymbol]);
        if (extraLen < 0)
            break;
        int length = kLengthBase[lenSymbol] + extraLen;
        if (length == 519)
            break;  // end-of-stream marker

        int distBits = (length == 2) ? 2 : dictBits;
        int distSymbol = decodeSymbol(reader, distTable);
        if (distSymbol < 0)
            break;
        int extraDist = reader.bits(distBits);
        if (extraDist < 0)
            break;
        int distance = (distSymbol << distBits) + extraDist + 1;

        if (static_cast<size_t>(distance) > out.size())
            break;  // impossible back-reference - bitstream has desynced from here on

        size_t from = out.size() - static_cast<size_t>(distance);
        for (int i = 0; i < length; i++)
            out += out[from + static_cast<size_t>(i)];
    }

    return out;
}

}  // namespace pkware

// Undoes the backslash-escaping every "@${[...]}" embedded-page payload is
// wrapped in (so it can live inside what's otherwise a null-terminated-
// string-oriented container): bytes < 0x20 are written as a backslash
// followed by a single base-32 digit ('0'-'9' then 'A'-'V' for 10-31).
// Confirmed on compressed (kind-5) payloads specifically, this needs two
// more rules beyond that base scheme: "\\" (a literal backslash following
// the escape backslash) decodes to one literal 0x5C byte, and "\Z" decodes
// to 0xFF. Both confirmed byte-exact against a live capture of Origin's
// own decompressor input (see the runbook) - all 5 testdata files decode
// the full report text with zero byte errors using this scheme.
std::string unescapeStorageBytes(const std::string& raw)
{
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size();)
    {
        unsigned char b = static_cast<unsigned char>(raw[i]);
        if (b == 0x5C && i + 1 < raw.size())
        {
            unsigned char c = static_cast<unsigned char>(raw[i + 1]);
            if (c >= '0' && c <= '9')
            {
                out += static_cast<char>(c - '0');
                i += 2;
                continue;
            }
            if (c >= 'A' && c <= 'V')
            {
                out += static_cast<char>(c - 'A' + 10);
                i += 2;
                continue;
            }
            if (c == '\\')
            {
                out += '\\';
                i += 2;
                continue;
            }
            if (c == 'Z')
            {
                out += static_cast<char>(0xFF);
                i += 2;
                continue;
            }
        }
        out += static_cast<char>(b);
        i += 1;
    }
    return out;
}

// Manual (non-regex, matching this file's style) parse for "Data
// Identifier: <value>" within a decoded note - the value runs to end of
// line. Used to match a decoded compressed note back to the workbook it
// belongs to (see decodeCompressedNotes() below).
std::string extractDataIdentifierFromDecodedNote(const std::string& decoded)
{
    const std::string marker = "Data Identifier: ";
    size_t pos = decoded.find(marker);
    if (pos == std::string::npos)
        return "";
    pos += marker.size();
    size_t end = decoded.find_first_of("\r\n", pos);
    return decoded.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
}

// Manual parse for "Park: <number>nm". Returns "" if not found, or if the
// text right after "Park: " isn't cleanly digits/'.' immediately followed
// by "nm" - which in practice is exactly what happens once the decoded
// text has run into the not-yet-resolved corruption described above, so
// this doubles as the "did decode actually make it this far intact" check.
std::string extractParkWavelengthText(const std::string& decoded)
{
    const std::string marker = "Park: ";
    size_t pos = decoded.find(marker);
    if (pos == std::string::npos)
        return "";
    pos += marker.size();
    size_t end = pos;
    while (end < decoded.size() &&
           (std::isdigit(static_cast<unsigned char>(decoded[end])) || decoded[end] == '.'))
        end++;
    if (end == pos || decoded.compare(end, 2, "nm") != 0)
        return "";
    return decoded.substr(pos, end - pos);
}

// Manual parse for "XBin:<digits>" (no space after the colon here, unlike
// the other fields - matches Aqualog's literal formatting). "" if not
// found or the digit run is empty.
std::string extractXBinText(const std::string& decoded)
{
    const std::string marker = "XBin:";
    size_t pos = decoded.find(marker);
    if (pos == std::string::npos)
        return "";
    pos += marker.size();
    size_t end = pos;
    while (end < decoded.size() && std::isdigit(static_cast<unsigned char>(decoded[end])))
        end++;
    if (end == pos)
        return "";
    return decoded.substr(pos, end - pos);
}

// Manual parse for "Integration Time: <seconds>" - the report's own
// pre-formatted decimal string, used instead of the <IntegrationTime> XML
// tag (see the big comment above ExpSummaryFields) because this one
// doesn't have binary floating-point representation noise. "" if not
// found, or if the text right after the marker isn't cleanly digits/'.'
// running straight to end of line - same "did decode survive to here"
// check as extractParkWavelengthText() above.
std::string extractIntegrationTimeText(const std::string& decoded)
{
    const std::string marker = "Integration Time: ";
    size_t pos = decoded.find(marker);
    if (pos == std::string::npos)
        return "";
    pos += marker.size();
    size_t end = pos;
    while (end < decoded.size() &&
           (std::isdigit(static_cast<unsigned char>(decoded[end])) || decoded[end] == '.'))
        end++;
    if (end == pos || (end < decoded.size() && decoded[end] != '\r' && decoded[end] != '\n'))
        return "";
    return decoded.substr(pos, end - pos);
}

// Manual parse for the rest of the line after `marker` - used for "ADC: "
// and "Gain: ", which are freeform descriptive text (e.g. "500 kHz G",
// "ADC Gain / 1.00") rather than a clean number, so unlike Park/XBin these
// are kept as strings. Rejects (returns "") if the line contains anything
// outside printable ASCII/CR/LF/TAB - decode() already stops at a hard
// bitstream failure, but text can still come out garbled-yet-printable
// before that point (see the big comment above the pkware:: namespace),
// and a stray non-printable byte is a cheap, reliable tell that this
// particular line was pulled from the garbled part.
std::string extractLineAfterMarker(const std::string& decoded, const std::string& marker)
{
    size_t pos = decoded.find(marker);
    if (pos == std::string::npos)
        return "";
    pos += marker.size();
    size_t end = decoded.find_first_of("\r\n", pos);
    std::string value = decoded.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    for (unsigned char c : value)
    {
        if (c != '\t' && (c < 0x20 || c > 0x7E))
            return "";
    }
    return value;
}

// Mirrors a MATLAB analysis script's CCD_gain lookup table: on this
// instrument, the physical ADC gain multiplier isn't given cleanly by
// either the "ADC: " or "Gain: " line alone - it's determined by their
// specific combination. `adcText`/`gainText` are the values already
// extracted via extractLineAfterMarker() above (without the "ADC: "/
// "Gain: " prefix). The numeric part of "Gain: ADC Gain / X.XX" is
// rounded to the nearest integer before matching (the MATLAB source
// required an exact "1.00"/"2.00" string match; rounding instead means a
// reading like "2.01" is still treated as the "2" case rather than falling
// through to unrecognized). Returns NaN for any combination that still
// doesn't match after rounding - callers should only treat that as a real
// "unrecognized combination" result, not "decode failed", by checking
// adcText/gainText are both non-empty first.
double ccdGainFactorFromReport(const std::string& adcText, const std::string& gainText)
{
    const std::string marker = "ADC Gain / ";
    size_t pos = gainText.find(marker);
    if (pos == std::string::npos)
        return kNaN;

    double gainValue;
    try
    {
        gainValue = std::stod(gainText.substr(pos + marker.size()));
    }
    catch (const std::exception&)
    {
        return kNaN;
    }
    long roundedGain = std::lround(gainValue);

    if (adcText == "500 kHz R" && roundedGain == 1)
        return 1.0;
    if (adcText == "500 kHz G" && roundedGain == 1)
        return 2.0;
    if (adcText == "500 kHz G" && roundedGain == 2)
        return 4.0;
    return kNaN;
}

struct CompressedNoteCandidate
{
    std::string dataIdentifier;  // "" if not found (decode failed too early, or this slot is empty)
    std::string decodedText;
};

// Scans the raw file bytes for every compressed ("kind 5")
// "_Storage_Ebdded_pages_Data_" record and best-effort decodes each one.
// One file can (and typically does) hold many of these - one per
// workbook-template sample-note slot, most of them empty leftovers from
// other samples that reused the same template - so the caller matches the
// result back to a specific workbook by comparing CompressedNoteCandidate::
// dataIdentifier against that workbook's own (already-reliable, XML-based)
// data identifier, rather than assuming a fixed position.
std::vector<CompressedNoteCandidate> decodeCompressedNotes(const std::string& fileBytes)
{
    std::vector<CompressedNoteCandidate> candidates;

    const std::string marker = "@${[0|5|_Storage_Ebdded_pages_Data_|";
    size_t pos = 0;
    while ((pos = fileBytes.find(marker, pos)) != std::string::npos)
    {
        size_t fieldsEnd = fileBytes.find("]}", pos);
        if (fieldsEnd == std::string::npos)
            break;

        std::string sizeField = fileBytes.substr(pos + marker.size(), fieldsEnd - (pos + marker.size()));
        size_t bar = sizeField.find('|');
        unsigned long size = 0;
        try
        {
            size = std::stoul(bar == std::string::npos ? sizeField : sizeField.substr(0, bar));
        }
        catch (const std::exception&)
        {
            pos = fieldsEnd + 2;
            continue;
        }

        size_t payloadStart = fieldsEnd + 2;
        if (payloadStart + size > fileBytes.size())
            break;

        std::string decoded = pkware::decode(unescapeStorageBytes(fileBytes.substr(payloadStart, size)));

        CompressedNoteCandidate candidate;
        candidate.dataIdentifier = extractDataIdentifierFromDecodedNote(decoded);
        candidate.decodedText = std::move(decoded);
        candidates.push_back(std::move(candidate));

        pos = payloadStart + size;
    }

    return candidates;
}

// Reads a whole file into memory (used to scan for compressed-note storage
// records directly in the raw bytes, alongside liborigin's own parse of
// the same file).
std::string readRawFileBytes(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Formats a time_t as an ISO-8601 UTC string, e.g. "2024-03-14T09:41:02Z".
// Returns an empty string for an unset/zero timestamp.
std::string formatTimestampUtc(time_t t)
{
    if (t <= 0)
        return "";
    std::tm* tmUtc = std::gmtime(&t);  // single-threaded CLI tool - fine to use the non-reentrant form
    if (!tmUtc)
        return "";
    std::ostringstream oss;
    oss << std::put_time(tmUtc, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

Origin::SpreadSheet* findSheet(Origin::Excel& book, const std::string& normalizedKey)
{
    for (auto& sheet : book.sheets)
    {
        if (normalizeSheetName(sheet.name) == normalizedKey)
            return &sheet;
    }
    return nullptr;
}

// Returns the raw row indices of `axisColumn` whose value falls inside the
// instrument's physical wavelength range, reordered so the values are
// ascending. This is how padded/unused rows get excluded, and how the
// reversed-vs-forward row order in different sheet types gets normalized
// to a single convention.
std::vector<size_t> validAscendingRowOrder(Origin::SpreadColumn& axisColumn)
{
    std::vector<size_t> valid;
    valid.reserve(axisColumn.data.size());
    for (size_t r = 0; r < axisColumn.data.size(); r++)
    {
        double v = variantToDouble(axisColumn.data[r]);
        if (v >= kMinPhysicalWavelength && v <= kMaxPhysicalWavelength)
            valid.push_back(r);
    }

    if (valid.size() >= 2 &&
        variantToDouble(axisColumn.data[valid.front()]) >
            variantToDouble(axisColumn.data[valid.back()]))
    {
        std::reverse(valid.begin(), valid.end());  // raw data was descending - flip to ascending
    }

    return valid;
}

// Reads `col` at the given raw row indices, in order. Keeps every column's
// data aligned to the same filtered/reordered row list as the axis column
// it belongs with.
std::vector<double> gatherValues(Origin::SpreadColumn& col, const std::vector<size_t>& rows)
{
    std::vector<double> out(rows.size(), kNaN);
    for (size_t i = 0; i < rows.size(); i++)
    {
        if (rows[i] < col.data.size())
            out[i] = variantToDouble(col.data[rows[i]]);
    }
    return out;
}

// Appends a numeric suffix if `baseName` is already present in `usedNames`
// (populated as groups are created). Checking against this in-memory set
// is O(1) on average, unlike repeatedly querying the NetCDF file's
// existing groups.
std::string uniqueGroupName(std::unordered_set<std::string>& usedNames, const std::string& baseName)
{
    if (usedNames.insert(baseName).second)
        return baseName;

    for (int suffix = 2;; suffix++)
    {
        std::string candidate = baseName + "_" + std::to_string(suffix);
        if (usedNames.insert(candidate).second)
            return candidate;
    }
}

// Lower-cased file extension, e.g. ".opj" or ".ogw".
std::string lowerExtension(const fs::path& p)
{
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                    [](unsigned char c) { return std::tolower(c); });
    return ext;
}

// Collects every .opj/.ogw file directly inside `input` (not subfolders) -
// or just `input` itself, if it's already a single .opj/.ogw file - sorted
// for a deterministic processing order.
//
// .opj (a full project) and .ogw (a single workbook) are both written using
// the same classic Origin binary container - liborigin's parser doesn't
// look at the file extension at all, it just reads the magic version
// header directly (see OriginFile's constructor), so a .ogw parses through
// exactly the same OriginFile/OriginAnyParser path as a .opj containing one
// workbook. This function is the only place aqualog2nc itself filters by
// extension, so that's the only thing that needed to change to accept .ogw.
std::vector<fs::path> collectOpjFiles(const fs::path& input)
{
    std::vector<fs::path> files;

    auto isSupported = [](const fs::path& p) {
        std::string ext = lowerExtension(p);
        return ext == ".opj" || ext == ".ogw";
    };

    if (fs::is_regular_file(input))
    {
        if (isSupported(input))
            files.push_back(input);
        return files;
    }

    if (fs::is_directory(input))
    {
        for (const auto& entry : fs::directory_iterator(input))
        {
            if (entry.is_regular_file() && isSupported(entry.path()))
                files.push_back(entry.path());
        }
        std::sort(files.begin(), files.end());
    }

    return files;
}

// ---------------------------------------------------------------------
// Pre-export consistency validation
// ---------------------------------------------------------------------

struct ValidationResult
{
    bool ok = true;
    bool isBlank = false;  // true specifically when there's no S1Sample sheet at all
    std::vector<std::string> reasons;
};

// Checks that every axis-bearing sheet in the workbook agrees on the
// emission/excitation counts before anything is written. See the
// PRE-EXPORT CONSISTENCY VALIDATION note at the top of this file for the
// full list of checks.
ValidationResult validateWorkbookAxes(Origin::Excel& book)
{
    ValidationResult result;

    // A workbook with no S1Sample sheet at all is a blank measurement, not
    // a sample - S1Blank alone doesn't establish a real emission axis for
    // export purposes here, so it's skipped with its own distinct message
    // rather than folded into the generic "failed consistency checks" path.
    Origin::SpreadSheet* emSource = findSheet(book, "S1Sample");
    if (!emSource)
    {
        result.ok = false;
        result.isBlank = true;
        result.reasons.push_back(
            "no S1Sample sheet found - this workbook looks like a blank measurement, not a sample");
        return result;
    }

    if (emSource->columns.empty())
    {
        result.ok = false;
        result.reasons.push_back("S1Sample sheet has no columns");
        return result;
    }

    size_t emissionCount = validAscendingRowOrder(emSource->columns[0]).size();
    if (emissionCount == 0)
    {
        result.ok = false;
        result.reasons.push_back("emission axis has zero valid (in-range) rows");
        return result;
    }

    Origin::SpreadSheet* exSource = nullptr;
    for (const char* key : {"R1andR1cSample", "R1andR1cBlank", "AbsSpectrumSample", "AbsSpectrumBlank"})
    {
        exSource = findSheet(book, key);
        if (exSource && !exSource->columns.empty())
            break;
        exSource = nullptr;
    }

    if (!exSource)
    {
        result.ok = false;
        result.reasons.push_back("no R1andR1c/AbsSpectrum sheet found to establish the excitation axis");
        return result;
    }

    size_t excitationCount = validAscendingRowOrder(exSource->columns[0]).size();
    if (excitationCount == 0)
    {
        result.ok = false;
        result.reasons.push_back("excitation axis has zero valid (in-range) rows");
        return result;
    }

    // Check 1: S1Sample row count must match the emission axis.
    {
        size_t rowCount = validAscendingRowOrder(emSource->columns[0]).size();
        if (rowCount != emissionCount)
        {
            result.ok = false;
            result.reasons.push_back("S1Sample: " + std::to_string(rowCount) +
                                      " emission rows, expected " + std::to_string(emissionCount));
        }
    }

    // Check 2: EEM matrix sheets (S1Sample and, if present, S1Blank) -
    // column count must match the excitation axis.
    for (const char* key : {"S1Sample", "S1Blank"})
    {
        Origin::SpreadSheet* sheet = findSheet(book, key);
        if (!sheet || sheet->columns.empty())
            continue;

        size_t colCount = sheet->columns.size() - 1;
        if (colCount != excitationCount)
        {
            result.ok = false;
            result.reasons.push_back(std::string(key) + ": " + std::to_string(colCount) +
                                      " excitation columns, expected " + std::to_string(excitationCount));
        }
    }

    // Check 3: absorbance sheets - row count must match the excitation axis.
    for (const char* key : {"AbsSpectrumSample", "AbsSpectrumBlank"})
    {
        Origin::SpreadSheet* sheet = findSheet(book, key);
        if (!sheet || sheet->columns.empty())
            continue;

        size_t rowCount = validAscendingRowOrder(sheet->columns[0]).size();
        if (rowCount != excitationCount)
        {
            result.ok = false;
            result.reasons.push_back(std::string(key) + ": " + std::to_string(rowCount) +
                                      " absorbance rows, expected " + std::to_string(excitationCount));
        }
    }

    // Check 4 (and, since XCorrect is sourced from these same sheets,
    // check 6): R1andR1c sheets - row count must match the excitation axis.
    for (const char* key : {"R1andR1cSample", "R1andR1cBlank"})
    {
        Origin::SpreadSheet* sheet = findSheet(book, key);
        if (!sheet || sheet->columns.empty())
            continue;

        size_t rowCount = validAscendingRowOrder(sheet->columns[0]).size();
        if (rowCount != excitationCount)
        {
            result.ok = false;
            result.reasons.push_back(std::string(key) + ": " + std::to_string(rowCount) +
                                      " excitation rows, expected " + std::to_string(excitationCount));
        }
    }

    // Check 5: S1DarkandMCorrect sheets (MCorrect's source) - row count
    // must match the emission axis.
    for (const char* key : {"S1DarkandMcorrectSample", "S1DarkandMcorrectBlank"})
    {
        Origin::SpreadSheet* sheet = findSheet(book, key);
        if (!sheet || sheet->columns.empty())
            continue;

        size_t rowCount = validAscendingRowOrder(sheet->columns[0]).size();
        if (rowCount != emissionCount)
        {
            result.ok = false;
            result.reasons.push_back(std::string(key) + ": " + std::to_string(rowCount) +
                                      " emission rows, expected " + std::to_string(emissionCount));
        }
    }

    return result;
}

// ---------------------------------------------------------------------
// Sheet classification - the C++ equivalent of the MATLAB
// switch(sheetnames{k}) block
// ---------------------------------------------------------------------

enum class SheetKind
{
    Skip,                    // Plot/Graph/Contour/Waterfall - no exportable data
    MatrixSample,            // S1Sample   - emission x excitation matrix
    MatrixBlank,             // S1Blank
    ExcitationVectorSample,  // R1andR1cSample - R1, R1dark
    ExcitationVectorBlank,   // R1andR1cBlank  - R1, R1dark (XCorrect handled separately)
    EmissionVectorSample,    // S1DarkandMCorrectSample - S1Dark
    EmissionVectorBlank,     // S1DarkandMCorrectBlank  - S1Dark, MCorrect
    AbsSample,               // AbsSpectrumSample
    AbsBlank,                // AbsSpectrumBlank
    Unknown                  // anything not recognized - warn, don't crash
};

SheetKind classifySheet(const std::string& normalizedName)
{
    if (contains(normalizedName, "Plot") || contains(normalizedName, "Graph") ||
        contains(normalizedName, "Contour") || contains(normalizedName, "Waterfall"))
        return SheetKind::Skip;

    if (normalizedName == "S1Sample")                return SheetKind::MatrixSample;
    if (normalizedName == "S1Blank")                 return SheetKind::MatrixBlank;
    if (normalizedName == "R1andR1cSample")          return SheetKind::ExcitationVectorSample;
    if (normalizedName == "R1andR1cBlank")           return SheetKind::ExcitationVectorBlank;
    if (normalizedName == "S1DarkandMcorrectSample") return SheetKind::EmissionVectorSample;
    if (normalizedName == "S1DarkandMcorrectBlank")  return SheetKind::EmissionVectorBlank;
    if (normalizedName == "AbsSpectrumSample")       return SheetKind::AbsSample;
    if (normalizedName == "AbsSpectrumBlank")        return SheetKind::AbsBlank;

    return SheetKind::Unknown;
}

// ---------------------------------------------------------------------
// Per-sheet-type writers
// ---------------------------------------------------------------------

// S1Sample / S1Blank: emission x excitation matrix.
//   dat = cell2mat(worksheetData(:,2:end));
//   Xout.S1Sample(j,:,:) = flip(dat,2);   <- reverse column order
//
// netCDF stores dimensions in C order (first declared = slowest-varying),
// but MATLAB's ncread() reverses dimension order on read to keep indexing
// natural for its column-major arrays. Declaring (excitation, emission)
// here - the reverse of the natural reading order - means ncread() in
// MATLAB returns an array shaped (emission, excitation), matching how the
// Aqualog software displays it. The values array below is filled to match
// this declared order: excitation-major (outer), emission-minor (inner).
void writeEemMatrix(NcGroup& group, Origin::SpreadSheet& sheet,
                     const NcDim& emDim, const NcDim& exDim,
                     const std::vector<size_t>& emRows,
                     const std::string& varName)
{
    if (sheet.columns.size() < 2 || emDim.isNull() || exDim.isNull() || emRows.empty())
    {
        std::cerr << "  [warn] sheet '" << sheet.name
                  << "': too few columns, or missing emission/excitation axis - skipped\n";
        return;
    }

    const size_t rows = emRows.size();
    const size_t cols = static_cast<size_t>(exDim.getSize());

    if (sheet.columns.size() - 1 != cols)
    {
        std::cerr << "  [warn] sheet '" << sheet.name << "': has "
                  << (sheet.columns.size() - 1) << " data columns but the excitation axis has "
                  << cols << " values - check for a truncated/mismatched sheet\n";
    }

    std::vector<double> values(rows * cols, kNaN);
    for (size_t c = 0; c < cols; c++)
    {
        size_t srcCol = cols - c;  // reverse order -> ascending excitation wavelength
        if (srcCol >= sheet.columns.size())
            continue;

        auto& col = sheet.columns[srcCol];
        for (size_t r = 0; r < rows; r++)
        {
            size_t srcRow = emRows[r];
            if (srcRow < col.data.size())
                values[c * rows + r] = variantToDouble(col.data[srcRow]);
        }
    }

    NcVar intensity = group.addVar(varName, ncDouble, {exDim, emDim});
    intensity.putVar(values.data());
    intensity.putAtt("coordinates", "excitation emission");
}

// R1andR1cSample / R1andR1cBlank: one row per excitation wavelength.
// XCorrect (column 3, Blank only) is handled separately by writeXCorrect()
// since it's one of several redundant sources for the same quantity.
void writeExcitationVector(NcGroup& group, Origin::SpreadSheet& sheet,
                            const NcDim& exDim, const std::vector<size_t>& exRows,
                            const std::string& label)
{
    if (sheet.columns.size() < 3 || exDim.isNull() || exRows.empty())
    {
        std::cerr << "  [warn] sheet '" << sheet.name
                  << "': too few columns, or missing excitation axis - skipped\n";
        return;
    }

    std::vector<double> r1 = gatherValues(sheet.columns[1], exRows);
    NcVar r1Var = group.addVar("R1_" + label, ncDouble, exDim);
    r1Var.putVar(r1.data());

    double r1dark = sheet.columns[2].data.empty() ? kNaN : variantToDouble(sheet.columns[2].data[0]);
    group.putAtt("R1dark_" + label, ncDouble, r1dark);
}

// S1DarkandMCorrectSample / S1DarkandMCorrectBlank: one row per emission
// wavelength, no reversal. MCorrect has only ever had one source sheet
// (the Blank one), so it's written directly here rather than through the
// comparison routine used for XCorrect.
void writeEmissionVector(NcGroup& group, Origin::SpreadSheet& sheet,
                          const NcDim& emDim, const std::vector<size_t>& emRows,
                          const std::string& label)
{
    if (sheet.columns.size() < 2 || emDim.isNull() || emRows.empty())
    {
        std::cerr << "  [warn] sheet '" << sheet.name
                  << "': too few columns, or missing emission axis - skipped\n";
        return;
    }

    std::vector<double> dark = gatherValues(sheet.columns[1], emRows);
    NcVar darkVar = group.addVar("S1Dark_" + label, ncDouble, emDim);
    darkVar.putVar(dark.data());

    if (label == "Blank" && sheet.columns.size() > 2)
    {
        std::vector<double> MCorrect = gatherValues(sheet.columns[2], emRows);
        NcVar mVar = group.addVar("MCorrect", ncDouble, emDim);
        mVar.putVar(MCorrect.data());
    }
}

// AbsSpectrumSample / AbsSpectrumBlank: absorbance vs. wavelength, on the
// shared "excitation" axis. Only AbsI1/AbsI1dark are written - the sheet's
// R1, R1dark, and "horiba backup" columns were confirmed by manual
// cross-check to be identical to R1_Sample/R1_Blank/R1dark_Sample/
// R1dark_Blank (already written from the R1andR1c sheets), so they're
// intentionally not duplicated here.
void writeAbsSpectrum(NcGroup& group, Origin::SpreadSheet& sheet,
                       const NcDim& exDim, const std::vector<size_t>& exRows,
                       const std::string& label)
{
    if (sheet.columns.size() < 3 || exDim.isNull() || exRows.empty())
    {
        std::cerr << "  [warn] sheet '" << sheet.name
                  << "': fewer columns than expected, or missing excitation axis - skipped\n";
        return;
    }

    std::vector<double> v = gatherValues(sheet.columns[1], exRows);
    NcVar var = group.addVar("AbsI1_" + label, ncDouble, exDim);
    var.putVar(v.data());

    group.putAtt("AbsI1dark_" + label, ncDouble,
                 sheet.columns[2].data.empty() ? kNaN : variantToDouble(sheet.columns[2].data[0]));
}

// XCorrect: up to three sheets can each carry a copy of this correction
// factor - R1andR1cBlank (column 3), R1andR1cSample (column 3, if
// present), and AbsSpectrumBlank (column 5). Gathers whichever exist,
// compares them, and writes exactly one "XCorrect" variable per workbook.
struct XCorrectCandidate
{
    std::string source;
    std::vector<double> values;
};

void writeXCorrect(NcGroup& group, Origin::Excel& book,
                    const NcDim& exDim, const std::vector<size_t>& exRows)
{
    if (exDim.isNull() || exRows.empty())
        return;

    std::vector<XCorrectCandidate> candidates;

    // Order here is also the priority order used to pick the written
    // value: R1andR1cBlank first, matching aqualogimport.m's Xout.XCorrect.
    auto tryAdd = [&](const char* sheetKey, size_t colIndex) {
        Origin::SpreadSheet* sheet = findSheet(book, sheetKey);
        if (!sheet || sheet->columns.size() <= colIndex)
            return;
        std::vector<double> v = gatherValues(sheet->columns[colIndex], exRows);
        bool anyValid = std::any_of(v.begin(), v.end(), [](double x) { return !std::isnan(x); });
        if (anyValid)
            candidates.push_back({sheetKey, std::move(v)});
    };

    tryAdd("R1andR1cBlank", 3);
    tryAdd("R1andR1cSample", 3);
    tryAdd("AbsSpectrumBlank", 5);

    if (candidates.empty())
        return;

    double maxDiff = 0.0;
    for (size_t i = 1; i < candidates.size(); i++)
    {
        for (size_t r = 0; r < exRows.size(); r++)
        {
            double a = candidates[0].values[r];
            double b = candidates[i].values[r];
            if (std::isnan(a) || std::isnan(b))
                continue;
            maxDiff = std::max(maxDiff, std::fabs(a - b));
        }
    }

    if (candidates.size() > 1 && maxDiff > kXCorrectTolerance)
    {
        std::cerr << "  [warn] workbook '" << book.label << "': XCorrect sources disagree by up to "
                  << maxDiff << "\n";
    }

    NcVar var = group.addVar("XCorrect", ncDouble, exDim);
    var.putVar(candidates.front().values.data());
}

// ---------------------------------------------------------------------
// Builds the shared per-workbook axes before any sheet data is written.
// ---------------------------------------------------------------------

struct WorkbookAxes
{
    NcDim emDim;
    NcDim exDim;
    std::vector<size_t> emRows;
    std::vector<size_t> exRows;
};

// Called only after validateWorkbookAxes() has confirmed the workbook is
// well-formed - in particular, that S1Sample exists - so no S1Blank
// fallback is needed here for the emission axis.
WorkbookAxes buildAxes(NcGroup& group, Origin::Excel& book)
{
    WorkbookAxes axes;

    Origin::SpreadSheet* emSource = findSheet(book, "S1Sample");

    if (emSource && !emSource->columns.empty())
    {
        axes.emRows = validAscendingRowOrder(emSource->columns[0]);
        if (!axes.emRows.empty())
        {
            std::vector<double> emission = gatherValues(emSource->columns[0], axes.emRows);
            axes.emDim = group.addDim("emission", emission.size());
            NcVar emVar = group.addVar("emission", ncDouble, axes.emDim);
            emVar.putAtt("units", "nm");
            emVar.putVar(emission.data());
        }
    }

    for (const char* key : {"R1andR1cSample", "R1andR1cBlank", "AbsSpectrumSample", "AbsSpectrumBlank"})
    {
        Origin::SpreadSheet* sheet = findSheet(book, key);
        if (!sheet || sheet->columns.empty())
            continue;

        std::vector<size_t> rows = validAscendingRowOrder(sheet->columns[0]);
        if (rows.empty())
            continue;

        axes.exRows = rows;
        std::vector<double> excitation = gatherValues(sheet->columns[0], rows);
        axes.exDim = group.addDim("excitation", excitation.size());
        NcVar exVar = group.addVar("excitation", ncDouble, axes.exDim);
        exVar.putAtt("units", "nm");
        exVar.putVar(excitation.data());
        break;
    }

    if (!axes.exDim.isNull() && emSource)
    {
        size_t matrixCols = emSource->columns.size() - 1;
        if (matrixCols != static_cast<size_t>(axes.exDim.getSize()))
        {
            std::cerr << "  [warn] workbook '" << book.label << "': EEM matrix has "
                      << matrixCols << " columns but the excitation axis has "
                      << axes.exDim.getSize() << " values - under the combined protocol these "
                      << "should match; double check this workbook\n";
        }
    }

    return axes;
}

// Exports every recognized sheet of one workbook into `group`.
void exportWorkbook(NcGroup& group, Origin::Excel& book)
{
    WorkbookAxes axes = buildAxes(group, book);
    writeXCorrect(group, book, axes.exDim, axes.exRows);

    for (auto& sheet : book.sheets)
    {
        std::string key = normalizeSheetName(sheet.name);
        SheetKind kind = classifySheet(key);

        try
        {
            switch (kind)
            {
                case SheetKind::Skip:
                    break;
                case SheetKind::MatrixSample:
                    writeEemMatrix(group, sheet, axes.emDim, axes.exDim, axes.emRows, "S1Sample");
                    break;
                case SheetKind::MatrixBlank:
                    writeEemMatrix(group, sheet, axes.emDim, axes.exDim, axes.emRows, "S1Blank");
                    break;
                case SheetKind::ExcitationVectorSample:
                    writeExcitationVector(group, sheet, axes.exDim, axes.exRows, "Sample");
                    break;
                case SheetKind::ExcitationVectorBlank:
                    writeExcitationVector(group, sheet, axes.exDim, axes.exRows, "Blank");
                    break;
                case SheetKind::EmissionVectorSample:
                    writeEmissionVector(group, sheet, axes.emDim, axes.emRows, "Sample");
                    break;
                case SheetKind::EmissionVectorBlank:
                    writeEmissionVector(group, sheet, axes.emDim, axes.emRows, "Blank");
                    break;
                case SheetKind::AbsSample:
                    writeAbsSpectrum(group, sheet, axes.exDim, axes.exRows, "Sample");
                    break;
                case SheetKind::AbsBlank:
                    writeAbsSpectrum(group, sheet, axes.exDim, axes.exRows, "Blank");
                    break;
                case SheetKind::Unknown:
                    break;
            }
        }
        catch (const NcException& e)
        {
            std::cerr << "  [error] sheet '" << sheet.name << "' in workbook '" << book.label
                      << "': " << e.what() << "\n";
        }
    }
}

}  // namespace

int main(int argc, char* argv[])
{
    if (argc < 3)
    {
        std::cout << "Usage: aqualog2nc <input.opj | input.ogw | input_folder> output.nc\n";
        return 1;
    }

    fs::path inputPath(argv[1]);
    std::vector<fs::path> opjFiles = collectOpjFiles(inputPath);

    if (opjFiles.empty())
    {
        std::cerr << "No .opj/.ogw files found at '" << argv[1] << "'\n";
        return 1;
    }

    std::cout << "Found " << opjFiles.size() << " .opj/.ogw file(s)\n";

    try
    {
        NcFile nc(argv[2], NcFile::replace);
        nc.putAtt("Conventions", "CF-1.8");
        nc.putAtt("source_input", argv[1]);

        // Every variable in this program is fully populated immediately
        // after creation, so netCDF's default "pre-fill with a fill value,
        // then overwrite" behavior is pure redundant I/O here - safe to
        // disable unconditionally.
        int oldFillMode;
        nc.set_Fill(NC_NOFILL, &oldFillMode);

        std::unordered_set<std::string> usedGroupNames;
        unsigned int skippedBlankCount = 0;
        unsigned int skippedInvalidCount = 0;
        unsigned int skippedWrongTypeCount = 0;
        //unsigned int sampleIndex = 0;

        for (const auto& opjPath : opjFiles)
        {
            std::string opjPathStr = opjPath.string();
            // .ogw holds exactly one workbook, so the "Exporting sample: "
            // line below already identifies it - an extra "Reading ..."
            // line ahead of it is just noise when there are many .ogw
            // files (one per sample) rather than a handful of multi-sample
            // .opj projects.
            if (lowerExtension(opjPath) != ".ogw")
                std::cout << "Reading " << opjPathStr << "\n";

            OriginFile opj(opjPathStr);
            if (!opj.parse())
            {
                std::cerr << "  [error] could not parse '" << opjPathStr << "' - skipped\n";
                continue;
            }

            // Any standalone Note windows sitting in the project tree
            // outside a workbook (opj.noteCount()/opj.note()) - e.g. a
            // scratch note an operator left at the project level rather than
            // on a specific sample's own Note page. Attached to every
            // sample group from this file, since there's no single "right"
            // group to put project-level text on.
            std::vector<std::string> standaloneNotes;
            for (unsigned int n = 0; n < opj.noteCount(); n++)
            {
                Origin::Note& note = opj.note(n);
                if (!note.text.empty())
                    standaloneNotes.push_back(note.name + ": " + note.text);
            }

            // Decoded once per file (not per workbook) and matched to a
            // workbook below by data identifier - see decodeCompressedNotes()
            // above for why a fixed position can't be assumed.
            std::vector<CompressedNoteCandidate> compressedNotes =
                decodeCompressedNotes(readRawFileBytes(opjPathStr));

            for (unsigned int i = 0; i < opj.excelCount(); i++)
            {
                //auto sampleStart = std::chrono::steady_clock::now();

                Origin::Excel& book = opj.excel(i);

                std::string fullLabel = book.label.empty() ? book.name : book.label;

                // Origin's Aqualog naming template appends a fixed,
                // non-sample-specific descriptor after the identifier
                // (e.g. "AO22020 (01) - 3D Acquisition EEM 3D CCD -
                // Absorbance"). Only the part through the closing
                // parenthesis is the actual sample identifier; the full,
                // untrimmed label is still kept in the workbook_name
                // attribute below.
                std::string sampleId = fullLabel;
                size_t descriptorPos = fullLabel.find(")");
                if (descriptorPos != std::string::npos)
                    sampleId = fullLabel.substr(0, descriptorPos + 1);

                ExpSummaryFields expSummary = extractExpSummaryFields(book.rawPropertyBlock);
                if (expSummary.expType != kRequiredExperimentType)
                {
                    std::cout << "  Skipping '" << sampleId << "' - Experiment Type is '"
                              << (expSummary.expType.empty() ? "(none found)" : expSummary.expType)
                              << "', not '" << kRequiredExperimentType << "'\n";
                    skippedWrongTypeCount++;
                    continue;
                }

                ValidationResult validation = validateWorkbookAxes(book);
                if (!validation.ok)
                {
                    if (validation.isBlank)
                    {
                        std::cout << "  Skipping '" << sampleId
                                  << "' - no S1Sample sheet; this is a blank, not a sample\n";
                        skippedBlankCount++;
                    }
                    else
                    {
                        std::cout << "  Skipping sample: " << sampleId
                                  << " (failed consistency checks)\n";
                        skippedInvalidCount++;
                    }
                    for (auto& reason : validation.reasons)
                        std::cerr << "    [skip] " << reason << "\n";
                    continue;
                }

                std::string baseName = safeName(sampleId);
                std::string groupName = uniqueGroupName(usedGroupNames, baseName);

                std::cout << "  Exporting sample: " << sampleId;
                if (groupName != baseName)
                    std::cout << " (name collision - stored as '" << groupName << "')";
                std::cout << "\n";

                NcGroup group = nc.addGroup(groupName);
                group.putAtt("workbook_name", sampleId);
                group.putAtt("workbook_short_name", book.name);
                group.putAtt("source_opj_file", opjPathStr);

                std::string dataIdentifier = extractDataIdentifier(fullLabel);
                if (!dataIdentifier.empty())
                    group.putAtt("data_identifier", dataIdentifier);

                // Fields pulled from the compressed "GENERAL PARAMETERS:"
                // report - integration time, Park wavelength (EM1's fixed
                // park position), the CCD's X binning factor, ADC readout
                // rate, and gain - each written only if the matching
                // compressed note decoded far enough to reach it intact;
                // see the "Compressed Note text" section above. As of the
                // escape-scheme fix confirmed against a live Origin
                // capture (see the runbook), all 5 known testdata files
                // decode the full report cleanly, so in practice all five
                // attributes should be present together or not at all -
                // but still written independently/defensively in case a
                // file not yet seen exercises something these didn't.
                if (!dataIdentifier.empty())
                {
                    for (const auto& candidate : compressedNotes)
                    {
                        if (candidate.dataIdentifier != dataIdentifier)
                            continue;

                        std::string integrationTimeText = extractIntegrationTimeText(candidate.decodedText);
                        if (!integrationTimeText.empty())
                        {
                            try
                            {
                                group.putAtt("integration_time", ncDouble, std::stod(integrationTimeText));
                            }
                            catch (const std::exception&)
                            {
                            }
                        }

                        std::string parkText = extractParkWavelengthText(candidate.decodedText);
                        if (!parkText.empty())
                        {
                            try
                            {
                                group.putAtt("park_wavelength_nm", ncDouble, std::stod(parkText));
                            }
                            catch (const std::exception&)
                            {
                            }
                        }

                        std::string xBinText = extractXBinText(candidate.decodedText);
                        if (!xBinText.empty())
                        {
                            try
                            {
                                group.putAtt("ccd_xbin", ncInt, std::stoi(xBinText));
                            }
                            catch (const std::exception&)
                            {
                            }
                        }

                        std::string adcText = extractLineAfterMarker(candidate.decodedText, "ADC: ");
                        if (!adcText.empty())
                            group.putAtt("ccd_adc_readout", adcText);

                        std::string gainText = extractLineAfterMarker(candidate.decodedText, "Gain: ");
                        if (!gainText.empty())
                            group.putAtt("ccd_gain", gainText);

                        if (!adcText.empty() && !gainText.empty())
                            group.putAtt("ccd_gain_factor", ncDouble, ccdGainFactorFromReport(adcText, gainText));

                        break;
                    }
                }

                std::string created = formatTimestampUtc(book.creationDate);
                if (!created.empty())
                    group.putAtt("creation_time", created);

                std::string modified = formatTimestampUtc(book.modificationDate);
                if (!modified.empty())
                    group.putAtt("modification_time", modified);

                // expSummary.expType was already used above to filter this
                // sample in; expFilename is the other <ExpSummary> field
                // worth keeping as its own typed attribute (see
                // extractExpSummaryFields() above) - integration_time used
                // to come from here too, but now comes from the compressed
                // report instead (see the block above), which doesn't have
                // this XML tag's floating-point representation noise.
                if (!expSummary.expFilename.empty())
                    group.putAtt("experiment_file", expSummary.expFilename);

                for (size_t n = 0; n < standaloneNotes.size(); n++)
                    group.putAtt("project_note_" + std::to_string(n), standaloneNotes[n]);

                exportWorkbook(group, book);

                //auto sampleEnd = std::chrono::steady_clock::now();
                //auto elapsedMs =
                //    std::chrono::duration_cast<std::chrono::milliseconds>(sampleEnd - sampleStart).count();
                //sampleIndex++;
                //std::cout << "    (sample #" << sampleIndex << ", " << elapsedMs << " ms)\n";
            }
        }

        if (skippedBlankCount > 0 || skippedInvalidCount > 0 || skippedWrongTypeCount > 0)
        {
            std::cout << "Done (" << skippedBlankCount << " blank(s) skipped, "
                      << skippedInvalidCount << " sample(s) failed consistency checks, "
                      << skippedWrongTypeCount << " sample(s) skipped for wrong Experiment Type)\n";
        }
        else
        {
            std::cout << "Done\n";
        }
    }
    catch (const NcException& e)
    {
        std::cerr << "NetCDF error: " << e.what() << "\n";
        return 1;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
