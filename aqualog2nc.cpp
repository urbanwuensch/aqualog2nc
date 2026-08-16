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
// Runs in three phases (see "SAMPLE-INDEXED SCHEMA" below): every sample
// from every input file is extracted into memory first, then samples are
// grouped by exact measurement signature (excitation/emission/XCorrect/
// MCorrect/park_wavelength_nm/ccd_gain_factor/ccd_xbin/ccd_adc_readout/
// ccd_gain), then each group is written. A sample's own spectral data and
// what used to be its own per-sample scalar attributes are now variables
// indexed by a "data_identifier_i" dimension shared with everything else
// in its group - see SampleRecord and writeBucket() below.
//
// Dispatches on worksheet *type* instead of treating every sheet as a
// generic 2D matrix - mirrors the switch(sheetnames{k}) structure in
// aqualogimport.m. See the comments on each extract* function for the
// specific MATLAB lines they correspond to.
//
// SAMPLE-INDEXED SCHEMA
// Samples that share the exact same excitation axis, emission axis,
// XCorrect, MCorrect, park_wavelength_nm, ccd_gain_factor, ccd_xbin,
// ccd_adc_readout, and ccd_gain (compared value-for-value, not just axis
// length - see measurementSignatureKey()) are collected into one "bucket"
// and write those shared axes/corrections exactly once, rather than once
// per sample. Any difference in any of these fields means two samples
// don't genuinely share one measurement configuration, so they end up in
// different buckets/groups even if their excitation/emission axes match.
// If every sample in the input shares one measurement signature, the
// whole file has a single bucket and no NetCDF groups at all - dimensions
// and variables sit directly at the top level. If the input spans more
// than one signature, each gets its own NcGroup, named "measurement_type_N"
// (zero-padded to a consistent width), largest group first. Either way,
// within a bucket's scope every
// per-sample quantity - both the spectral arrays (S1Sample, R1_Blank, ...)
// and what used to be scalar attributes (integration_time, R1dark_Sample,
// data_identifier, ...) - is a variable indexed by a "data_identifier_i"
// dimension, not an attribute. data_identifier_i is a plain monotonically
// increasing integer index and doubles as that dimension's own CF
// coordinate variable, the same way excitation/emission are for theirs -
// data_identifier (the human-readable string) deliberately does not fill
// that role, since it's allowed to repeat across samples and a CF
// coordinate variable must be strictly monotonic (see kCfAttributes'
// comment on data_identifier_i for why). See SampleRecord (one per
// exported sample, holds everything before it's bucketed) and
// writeBucket() (writes one bucket's worth of variables, one indexed
// slice per sample).
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
// PER-SAMPLE VARIABLE DIMENSION ORDER (MATLAB compatibility)
// netCDF stores dimensions in C order (first declared = slowest-varying),
// but MATLAB's ncread()/netcdf.getVar() reverse dimension order on read to
// keep indexing natural for its column-major arrays. Every per-sample
// variable in writeBucket() - S1Sample/S1Blank (3D) and the 2D ones
// (R1_Sample/R1_Blank/AbsI1_Sample/AbsI1_Blank/S1Dark_Sample/
// S1Dark_Blank) - is therefore declared with "sample" LAST, not first
// (e.g. (excitation, emission, sample), not (sample, excitation,
// emission)), so that reading it back in MATLAB returns sample FIRST
// (e.g. (sample, emission, excitation)), consistent across every
// sample-indexed variable in this schema. Each record's own values are
// filled by extractEemMatrix() to
// match: excitation-major (outer), emission-minor (inner) - the (sample)
// index is added later, when writeBucket() writes that record's slice
// into the shared variable, as a size-1 hyperslab at that sample's
// position (a size-1 slab needs no reshaping of the record's own flat
// matrix regardless of which position in the dimension list it occupies).
//
// XCORRECT CONSOLIDATION
// Up to three sheets can each carry a copy of the excitation correction
// factor (R1andR1cBlank, R1andR1cSample, AbsSpectrumBlank) - these should
// all represent the same physical quantity. extractXCorrect() gathers
// whichever of these exist for a workbook, compares them, and fills in a
// single XCorrect vector (preferring R1andR1cBlank as the canonical
// source, matching aqualogimport.m's Xout.XCorrect). MCorrect has only
// ever had one source, so it's unchanged, still extracted per-workbook on
// the emission axis. Both are written once per bucket (see "SAMPLE-
// INDEXED SCHEMA" above), from whichever record happens to be first in
// the bucket - since bucketing is by exact XCorrect/MCorrect value, every
// record in a bucket has the same one anyway.
//
// REDUNDANT ABSORBANCE FIELDS DROPPED
// AbsSpectrumSample/Blank's R1, R1dark, and "horiba backup" columns were
// confirmed by manual cross-check against the Aqualog software to be
// identical to R1_Sample/R1_Blank/R1dark_Sample/R1dark_Blank (already
// extracted from the R1andR1c sheets), so extractAbsSpectrum() only fills
// in AbsI1_Sample/Blank and AbsI1dark_Sample/Blank - the values that are
// unique to the absorbance sheet.
//
// SAMPLE TIMESTAMPS
// Origin::Excel inherits Window::creationDate / Window::modificationDate
// (time_t), which Origin sets when the workbook is created/last modified.
// These become the "creation_time"/"modification_time" ISO 8601:2004
// extended-format string variables, one value per sample - see
// formatTimestampLocal()'s own comment for why they carry this machine's
// timezone offset rather than "Z"/UTC.
//
// SAMPLE IDENTITY
// Origin::Window::name is the short internal identifier (e.g. "Book1") -
// not what Origin displays, and not what aqualogimport.m uses - it
// becomes "workbook_short_name". Origin::Window::label is the long,
// human-readable name (with Origin's own "(01)" auto-numbering for
// reused base names), matching MATLAB's LongName - the full, untrimmed
// text becomes "workbook_name", and the label's first line (see
// extractDataIdentifier()) becomes "data_identifier". The "(NN)" counter
// is kept in both - it's part of what LongName actually contains, and
// nothing here second-guesses which part of the label "really" counts as
// the identifier. The label/LongName is the *only* source
// "data_identifier" is ever drawn from - deliberately: whatever the user
// currently has it set to in Origin's own workbook browser is trusted at
// face value, including any rename made after the fact, and it is never
// cross-checked against, or overridden by, anything found inside the
// compressed Note (see findCompressedNoteForWindow()'s comment for why -
// two workbooks' Notes can be entirely genuine and still byte-identical,
// so there is nothing in the Note's own text that could make it a more
// authoritative source of identity than the label). Since a NetCDF group
// no longer identifies an individual sample (see "SAMPLE-INDEXED SCHEMA"
// above), there's no need for data_identifier to be unique - two samples
// can freely share the same one; they're told apart by their position
// along the "data_identifier_i" dimension, not by the string value
// itself.
//
// One exception is flagged, not silently accepted: two *different* input
// .opj files can independently contain a workbook with the same label
// (counter included), which Origin's own "(NN)" auto-numbering has no
// way to catch (it only disambiguates reused names *within* one
// project). So after all files are collected, every data_identifier used
// by more than one source file gets "_duplicate" appended on every
// occurrence past the first file that used it (see the tagging pass in
// main(), right after Phase 1). This is purely an informational
// FAIR-findability flag, not a correctness gate - the sample's data is
// exported exactly as it would be otherwise.
//
// PRE-EXPORT CONSISTENCY VALIDATION
// Before anything is extracted for a workbook, validateWorkbookAxes()
// checks that every axis-bearing sheet agrees on the emission/excitation
// counts:
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
// truncated relative to others - the whole sample is skipped before it's
// ever collected into a SampleRecord, rather than exported with silent
// NaN gaps.
//
// BATCH MODE / SAMPLE NAME COLLISIONS
// Two different .opj files can each contain a workbook with the same name
// (e.g. both call it "Sample_1") - this is no longer a problem to solve,
// since a name is no longer used as a NetCDF group identifier (see
// "SAMPLE IDENTITY" above). Every sample also gets its own
// "source_opj_file" string variable so you can always trace it back to
// its origin file regardless of any name collision.
//
// PERFORMANCE OVER LARGE BATCHES (hundreds/thousands of samples)
// nc.set_Fill(NC_NOFILL, ...) disables netCDF's default pre-fill-then-
// overwrite behavior for new variables. Since every variable here is
// always fully populated immediately after creation, that pre-fill write
// is pure redundant I/O in this code's usage pattern. Every sample from
// every input file is held in memory at once (see "SAMPLE-INDEXED SCHEMA"
// above) before any NetCDF writing starts - fine even for datasets with
// several thousand samples, but worth knowing if you're running this
// against something far larger.
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
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace netCDF;
using namespace netCDF::exceptions;
namespace fs = std::filesystem;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// Missing-value sentinel for the fields extracted from the compressed
// Note report (integration_time, park_wavelength_nm, ccd_xbin,
// ccd_gain_factor, data_identifier, ccd_adc_readout, ccd_gain) - written
// (as -9999/-9999.0/"-9999") and declared via a "missing_value" attribute
// whenever the Note wasn't found, or didn't decode far enough to reach
// that specific field. Every other field (R1dark_*, AbsI1dark_*,
// workbook_name, experiment_file, ...) is sourced some other way (a
// spreadsheet, the workbook object itself) and keeps its own separate
// missing-value convention (NaN / "") - this sentinel is specific to
// Note-derived fields.
constexpr double kMissingValue = -9999.0;
constexpr int kMissingValueInt = -9999;
const std::string kMissingValueString = "-9999";

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
// Everything one exported sample contributes to the output file. Collected
// in full for every sample across every input file before anything is
// written to NetCDF - see organizeIntoBuckets()/writeBucket() below for
// why: samples that share the same excitation/emission/XCorrect/MCorrect/
// park_wavelength_nm/ccd_gain_factor/ccd_xbin/ccd_adc_readout/ccd_gain
// are grouped together and write those shared axes once, so the grouping
// can't be decided (and no NetCDF dimension can be sized) until every
// sample's data is already in hand.
// ---------------------------------------------------------------------
struct SampleRecord
{
    // Axes + corrections - drive the measurement-signature grouping (see
    // measurementSignatureKey()) and are written once per group of
    // samples that share them, not once per sample.
    std::vector<double> excitation, emission, xCorrect, mCorrect;

    // Spectral data, sized against excitation.size()/emission.size().
    std::vector<double> S1Sample, S1Blank;            // excitation * emission, flattened
    std::vector<double> R1_Sample, R1_Blank;          // excitation
    std::vector<double> AbsI1_Sample, AbsI1_Blank;    // excitation
    std::vector<double> S1Dark_Sample, S1Dark_Blank;  // emission

    // Per-sample scalars - were group attributes before this file's
    // group-per-sample layout was replaced with sample-indexed variables.
    double R1dark_Sample = kNaN, R1dark_Blank = kNaN;
    double AbsI1dark_Sample = kNaN, AbsI1dark_Blank = kNaN;
    // These four come from the compressed Note report, not a spreadsheet -
    // see kMissingValue's comment for why they default differently.
    double integrationTime = kMissingValue, parkWavelengthNm = kMissingValue, ccdGainFactor = kMissingValue;
    int ccdXBin = kMissingValueInt;

    // Per-sample strings - every one defaults to the same "-9999" sentinel
    // per "netcdf variable attributes.xlsx" 's missing_value column, kept
    // in sync with each variable's declared missing_value attribute (see
    // kCfAttributes/kMissingValueString). None of these ever actually
    // stays at this default in practice except the genuinely-optional
    // ones (ccdAdcReadout, ccdGain, experimentFile, creationTime,
    // modificationTime) - workbookName/workbookShortName/sourceOpjFile/
    // dataIdentifier are always populated directly from the Origin
    // object, but keep the same convention for consistency.
    std::string workbookName{kMissingValueString}, workbookShortName{kMissingValueString};
    std::string sourceOpjFile{kMissingValueString};
    std::string dataIdentifier{kMissingValueString};  // workbook label/LongName's first line, "(NN)" counter kept - see extractDataIdentifier()
    std::string ccdAdcReadout{kMissingValueString}, ccdGain{kMissingValueString};  // e.g. "500 kHz G" / "ADC Gain / 1.00"
    std::string experimentFile{kMissingValueString};
    std::string creationTime{kMissingValueString}, modificationTime{kMissingValueString};  // ISO 8601:2004 extended, this machine's tz offset
};

// ---------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------

// Keeps only letters, digits, and underscores (dropping parentheses
// entirely rather than substituting them, e.g. "(01)" -> "01"), collapses
// repeated underscores, and trims them from both ends. Used for NetCDF
// group/variable names, which are far stricter about allowed characters
// than Origin's own sample-naming conventions.
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
// comment text; "(NN)" is Origin's own per-workbook creation-order counter.
// It's kept, not stripped: it's part of what Origin's LongName property
// (and this codebase's MATLAB counterpart, workbookNameL = get(wbh,
// 'LongName')) actually contains, and honoring the label verbatim - the
// same principle "SAMPLE IDENTITY" above applies to the label as a whole -
// means not second-guessing which part of it "really" counts as the
// identifier.
std::string extractDataIdentifier(const std::string& label)
{
    size_t newline = label.find('\n');
    std::string firstLine = (newline == std::string::npos) ? label : label.substr(0, newline);
    while (!firstLine.empty() && (firstLine.back() == '\r' || firstLine.back() == ' '))
        firstLine.pop_back(); // labels use CRLF line endings
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
    std::string decodedText;
    // Byte offset, in the source file, of this record's own
    // "@${[0|5|..." marker - used to test structural containment within a
    // workbook's [dataStartOffset, dataEndOffset) range (see
    // findCompressedNoteForWindow()).
    size_t markerOffset = 0;
};

// Scans the raw file bytes for every compressed ("kind 5")
// "_Storage_Ebdded_pages_Data_" record and best-effort decodes each one.
// One file can (and typically does) hold many of these - one per
// workbook-template sample-note slot, most of them empty leftovers from
// other samples that reused the same template.
std::vector<CompressedNoteCandidate> decodeCompressedNotes(const std::string& fileBytes)
{
    std::vector<CompressedNoteCandidate> candidates;

    const std::string marker = "@${[0|5|_Storage_Ebdded_pages_Data_|";
    size_t pos = 0;
    while ((pos = fileBytes.find(marker, pos)) != std::string::npos)
    {
        size_t markerOffset = pos;
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
        candidate.decodedText = std::move(decoded);
        candidate.markerOffset = markerOffset;
        candidates.push_back(std::move(candidate));

        pos = payloadStart + size;
    }

    return candidates;
}

// Finds the Note that structurally belongs to a given workbook: the one
// whose own storage-record marker offset falls inside [dataStartOffset,
// dataEndOffset) - i.e. it's physically nested inside that workbook's own
// serialized data in the file, which is how Origin itself associates a
// Note with its owning workbook (see dataStartOffset's comment in
// OriginObj.h). No identifier-text comparison, duplicate-content
// detection, or any other content-based judgment is involved - the Note
// found this way is trusted unconditionally as a genuine record of this
// workbook's own acquisition. Two workbooks' Notes can be byte-identical
// (e.g. an operator ran two samples back to back without changing any
// setting, including the Data Identifier field, in Aqualog's own
// acquisition dialogue) without either one being any less genuine - the
// only thing that actually differs between such runs (their true
// measurement time) isn't part of the Note's own text at all, so content
// identity carries no information about which one is "real". Returns
// nullptr if no EM1-bearing record falls in range, or if more than one
// does (never observed, but ambiguous if it happened).
const CompressedNoteCandidate* findCompressedNoteForWindow(const std::vector<CompressedNoteCandidate>& compressedNotes,
                                                             long long dataStartOffset, long long dataEndOffset)
{
    const CompressedNoteCandidate* match = nullptr;
    for (const auto& candidate : compressedNotes)
    {
        if (static_cast<long long>(candidate.markerOffset) < dataStartOffset ||
            static_cast<long long>(candidate.markerOffset) >= dataEndOffset)
            continue;
        if (candidate.decodedText.find("\r\nEM1:") == std::string::npos)
            continue;
        if (match)
            return nullptr;  // more than one - ambiguous, shouldn't happen
        match = &candidate;
    }
    return match;
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

// Portable current-timezone UTC offset (in seconds, positive = east of
// UTC) for a given instant. Deliberately not tm_gmtoff (a BSD/glibc
// extension - MSVC's CRT doesn't provide it, and this project ships a
// Windows build via vcpkg): reinterpret t's own UTC broken-down fields
// as if they were local time via mktime(), then diff against t. Uses
// t's own calendar date (not "now") so DST resolves correctly for
// whichever date is being formatted, per this machine's own timezone
// rules.
long localUtcOffsetSecondsFor(time_t t)
{
    std::tm utcTm = *std::gmtime(&t);  // single-threaded CLI tool - fine to use the non-reentrant form
    utcTm.tm_isdst = -1;
    time_t asIfLocal = std::mktime(&utcTm);
    return static_cast<long>(std::difftime(t, asIfLocal));
}

// Formats a time_t (from Origin's own stored Julian-date value - see
// doubleToPosixTime() in OriginAnyParser.h) as an ISO 8601:2004 extended
// date-time string, e.g. "2024-03-14T09:41:02+02:00" - per ACDD's
// date/time convention, which requires ISO 8601. Returns an empty
// string for an unset/zero timestamp.
//
// Origin's stored timestamp carries no timezone of its own - it's the
// acquiring computer's bare wall-clock reading (year/month/day/hour/
// minute/second), run through a pure Julian-date-to-Unix-epoch formula
// with no UTC normalization ever applied - so gmtime() on the resulting
// time_t reproduces exactly those original wall-clock fields, not a
// genuine UTC instant. There is therefore no way to know for certain
// what timezone that reading was taken in. Rather than a "Z" suffix -
// which would falsely claim a certainty about the timezone that doesn't
// exist - this machine's own current timezone offset is appended as the
// best available stand-in, per the user's explicit decision on how to
// handle this ambiguity.
std::string formatTimestampLocal(time_t t)
{
    if (t <= 0)
        return "";
    std::tm* tmPtr = std::gmtime(&t);  // single-threaded CLI tool - fine to use the non-reentrant form
    if (!tmPtr)
        return "";
    std::tm recordedTm = *tmPtr;

    long offsetSeconds = localUtcOffsetSecondsFor(t);
    char sign = offsetSeconds < 0 ? '-' : '+';
    long absOffset = std::labs(offsetSeconds);

    std::ostringstream oss;
    oss << std::put_time(&recordedTm, "%Y-%m-%dT%H:%M:%S");
    oss << sign << std::setw(2) << std::setfill('0') << (absOffset / 3600) << ':'
        << std::setw(2) << std::setfill('0') << ((absOffset % 3600) / 60);
    return oss.str();
}

// Formats an elapsed duration (in seconds) as an ISO 8601:2004 duration
// string, e.g. "P3DT2H30M15S" - for "time_coverage_duration", computed
// as the plain difference between two time_t instants (not a
// calendar-based duration), so this deliberately only ever uses the
// D/H/M/S components, never Y/M(onths) - those are ambiguous for an
// exact elapsed-seconds value (months/years vary in length) in a way
// days/hours/minutes/seconds aren't. Zero-valued components are omitted
// per the standard, except the special case of an exactly-zero duration
// ("PT0S", ISO 8601's own convention - there's no shorter valid form).
std::string formatIsoDuration(time_t totalSeconds)
{
    if (totalSeconds < 0)
        totalSeconds = 0;
    long days = static_cast<long>(totalSeconds / 86400);
    long rem = static_cast<long>(totalSeconds % 86400);
    long hours = rem / 3600;
    rem %= 3600;
    long minutes = rem / 60;
    long secs = rem % 60;

    std::ostringstream oss;
    oss << "P";
    if (days > 0)
        oss << days << "D";
    if (hours > 0 || minutes > 0 || secs > 0)
    {
        oss << "T";
        if (hours > 0)
            oss << hours << "H";
        if (minutes > 0)
            oss << minutes << "M";
        if (secs > 0)
            oss << secs << "S";
    }
    if (days == 0 && hours == 0 && minutes == 0 && secs == 0)
        oss << "T0S";
    return oss.str();
}

// One "history" entry: the instant it represents (for chronological
// sorting - see the sort site near where "history" is written) and the
// fully-formatted line text. sortKey is deliberately not derived from
// the formatted text (comparing ISO 8601 strings with different UTC
// offsets - e.g. a winter +01:00 measurement timestamp next to a summer
// +02:00 processing timestamp - is not guaranteed to agree with true
// chronological order), so every entry carries its own raw time_t.
struct DiagnosticEntry
{
    time_t sortKey;
    std::string line;
};

// Appends one entry to the diagnostic log that ends up in the output
// file's "history" attribute, prefixed with the current date/time and
// this program's name - per the NetCDF Users Guide's "history"
// convention (the one both CF §2.6.2 and ACDD point to): a line for
// each invocation/action, each carrying its own date/time and program
// name. See logInvocation() for the one line that also carries the
// user name and command arguments, satisfying the rest of that
// convention without repeating them on every single line. Sorted by
// "now" at the time this was called - the right sort key for anything
// this program logs about its own actions (as opposed to a fact about
// the data itself - see the "completed measurement" entries in main(),
// which use their own measurement's completion time instead, so they
// sort chronologically before this program's own, much later,
// processing-time entries).
void logDiagnostic(std::vector<DiagnosticEntry>& diagnosticLog, const std::string& message)
{
    time_t now = std::time(nullptr);
    diagnosticLog.push_back({now, formatTimestampLocal(now) + " aqualog2nc: " + message});
}

// Portable current username lookup (POSIX sets USER, Windows sets
// USERNAME) - used once, for the invocation line logInvocation() writes.
std::string currentUsername()
{
    const char* user = std::getenv("USER");
    if (!user || !*user)
        user = std::getenv("USERNAME");
    return (user && *user) ? user : "unknown";
}

// The one "history" line that on its own satisfies the full NetCDF
// Users Guide convention - date, time, user name, program name, and
// command arguments - so every other logDiagnostic() entry only needs
// its own date/time and program name, since they're all part of this
// same invocation.
void logInvocation(std::vector<DiagnosticEntry>& diagnosticLog, int argc, char** argv)
{
    time_t now = std::time(nullptr);
    std::string line = formatTimestampLocal(now) + " " + currentUsername() + " aqualog2nc";
    for (int i = 1; i < argc; i++)
        line += " " + std::string(argv[i]);
    diagnosticLog.push_back({now, line});
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
// Per-sheet-type extractors - fill in a SampleRecord's fields instead of
// writing directly to NetCDF. Writing happens later, once per bucket of
// samples that share the same measurement signature (see
// measurementSignatureKey()/writeBucket()), not once per sample.
// ---------------------------------------------------------------------

// S1Sample / S1Blank: emission x excitation matrix.
//   dat = cell2mat(worksheetData(:,2:end));
//   Xout.S1Sample(j,:,:) = flip(dat,2);   <- reverse column order
//
// netCDF stores dimensions in C order (first declared = slowest-varying),
// but MATLAB's ncread() reverses dimension order on read to keep indexing
// natural for its column-major arrays. writeBucket() declares
// (sample, excitation, emission) - so that ncread() in MATLAB returns an
// array shaped (emission, excitation, sample), matching how the Aqualog
// software displays each sample's own matrix. The values array below is
// filled to match: excitation-major (outer), emission-minor (inner) -
// the (sample) dimension is added later, when this record's slice is
// written into the shared variable.
void extractEemMatrix(Origin::SpreadSheet& sheet, const std::vector<size_t>& emRows,
                       size_t exCount, const std::string& varName, SampleRecord& record)
{
    if (sheet.columns.size() < 2 || exCount == 0 || emRows.empty())
    {
        std::cerr << "  [warn] sheet '" << sheet.name
                  << "': too few columns, or missing emission/excitation axis - skipped\n";
        return;
    }

    const size_t rows = emRows.size();
    const size_t cols = exCount;

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

    if (varName == "S1Sample")
        record.S1Sample = std::move(values);
    else
        record.S1Blank = std::move(values);
}

// R1andR1cSample / R1andR1cBlank: one row per excitation wavelength.
// XCorrect (column 3, Blank only) is handled separately by
// extractXCorrect() since it's one of several redundant sources for the
// same quantity.
void extractExcitationVector(Origin::SpreadSheet& sheet, const std::vector<size_t>& exRows,
                              const std::string& label, SampleRecord& record)
{
    if (sheet.columns.size() < 3 || exRows.empty())
    {
        std::cerr << "  [warn] sheet '" << sheet.name
                  << "': too few columns, or missing excitation axis - skipped\n";
        return;
    }

    std::vector<double> r1 = gatherValues(sheet.columns[1], exRows);
    double r1dark = sheet.columns[2].data.empty() ? kNaN : variantToDouble(sheet.columns[2].data[0]);

    if (label == "Sample")
    {
        record.R1_Sample = std::move(r1);
        record.R1dark_Sample = r1dark;
    }
    else
    {
        record.R1_Blank = std::move(r1);
        record.R1dark_Blank = r1dark;
    }
}

// S1DarkandMCorrectSample / S1DarkandMCorrectBlank: one row per emission
// wavelength, no reversal. MCorrect has only ever had one source sheet
// (the Blank one), so it's filled in directly here rather than through
// the comparison routine used for XCorrect.
void extractEmissionVector(Origin::SpreadSheet& sheet, const std::vector<size_t>& emRows,
                            const std::string& label, SampleRecord& record)
{
    if (sheet.columns.size() < 2 || emRows.empty())
    {
        std::cerr << "  [warn] sheet '" << sheet.name
                  << "': too few columns, or missing emission axis - skipped\n";
        return;
    }

    std::vector<double> dark = gatherValues(sheet.columns[1], emRows);
    if (label == "Sample")
        record.S1Dark_Sample = std::move(dark);
    else
        record.S1Dark_Blank = std::move(dark);

    if (label == "Blank" && sheet.columns.size() > 2)
        record.mCorrect = gatherValues(sheet.columns[2], emRows);
}

// AbsSpectrumSample / AbsSpectrumBlank: absorbance vs. wavelength, on the
// shared "excitation" axis. Only AbsI1/AbsI1dark are extracted - the
// sheet's R1, R1dark, and "horiba backup" columns were confirmed by
// manual cross-check to be identical to R1_Sample/R1_Blank/R1dark_Sample/
// R1dark_Blank (already extracted from the R1andR1c sheets), so they're
// intentionally not duplicated here.
void extractAbsSpectrum(Origin::SpreadSheet& sheet, const std::vector<size_t>& exRows,
                         const std::string& label, SampleRecord& record)
{
    if (sheet.columns.size() < 3 || exRows.empty())
    {
        std::cerr << "  [warn] sheet '" << sheet.name
                  << "': fewer columns than expected, or missing excitation axis - skipped\n";
        return;
    }

    std::vector<double> v = gatherValues(sheet.columns[1], exRows);
    double dark = sheet.columns[2].data.empty() ? kNaN : variantToDouble(sheet.columns[2].data[0]);

    if (label == "Sample")
    {
        record.AbsI1_Sample = std::move(v);
        record.AbsI1dark_Sample = dark;
    }
    else
    {
        record.AbsI1_Blank = std::move(v);
        record.AbsI1dark_Blank = dark;
    }
}

// XCorrect: up to three sheets can each carry a copy of this correction
// factor - R1andR1cBlank (column 3), R1andR1cSample (column 3, if
// present), and AbsSpectrumBlank (column 5). Gathers whichever exist,
// compares them, and fills in exactly one XCorrect vector per workbook.
struct XCorrectCandidate
{
    std::string source;
    std::vector<double> values;
};

void extractXCorrect(Origin::Excel& book, const std::vector<size_t>& exRows, SampleRecord& record)
{
    if (exRows.empty())
        return;

    std::vector<XCorrectCandidate> candidates;

    // Order here is also the priority order used to pick the extracted
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

    record.xCorrect = candidates.front().values;
}

// ---------------------------------------------------------------------
// Extracts the shared per-workbook axes before any sheet data is read.
// ---------------------------------------------------------------------

struct WorkbookAxes
{
    std::vector<size_t> emRows;
    std::vector<size_t> exRows;
};

// Called only after validateWorkbookAxes() has confirmed the workbook is
// well-formed - in particular, that S1Sample exists - so no S1Blank
// fallback is needed here for the emission axis.
WorkbookAxes extractAxes(Origin::Excel& book, SampleRecord& record)
{
    WorkbookAxes axes;

    Origin::SpreadSheet* emSource = findSheet(book, "S1Sample");

    if (emSource && !emSource->columns.empty())
    {
        axes.emRows = validAscendingRowOrder(emSource->columns[0]);
        if (!axes.emRows.empty())
            record.emission = gatherValues(emSource->columns[0], axes.emRows);
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
        record.excitation = gatherValues(sheet->columns[0], rows);
        break;
    }

    if (!record.excitation.empty() && emSource)
    {
        size_t matrixCols = emSource->columns.size() - 1;
        if (matrixCols != record.excitation.size())
        {
            std::cerr << "  [warn] workbook '" << book.label << "': EEM matrix has "
                      << matrixCols << " columns but the excitation axis has "
                      << record.excitation.size() << " values - under the combined protocol these "
                      << "should match; double check this workbook\n";
        }
    }

    return axes;
}

// Extracts every recognized sheet of one workbook into a fresh SampleRecord.
// Only the axes/spectral/XCorrect/MCorrect fields are filled in here - the
// caller (main()) fills in the identity and report-derived fields.
SampleRecord extractWorkbook(Origin::Excel& book)
{
    SampleRecord record;
    WorkbookAxes axes = extractAxes(book, record);
    extractXCorrect(book, axes.exRows, record);

    for (auto& sheet : book.sheets)
    {
        std::string key = normalizeSheetName(sheet.name);
        SheetKind kind = classifySheet(key);

        switch (kind)
        {
            case SheetKind::Skip:
                break;
            case SheetKind::MatrixSample:
                extractEemMatrix(sheet, axes.emRows, axes.exRows.size(), "S1Sample", record);
                break;
            case SheetKind::MatrixBlank:
                extractEemMatrix(sheet, axes.emRows, axes.exRows.size(), "S1Blank", record);
                break;
            case SheetKind::ExcitationVectorSample:
                extractExcitationVector(sheet, axes.exRows, "Sample", record);
                break;
            case SheetKind::ExcitationVectorBlank:
                extractExcitationVector(sheet, axes.exRows, "Blank", record);
                break;
            case SheetKind::EmissionVectorSample:
                extractEmissionVector(sheet, axes.emRows, "Sample", record);
                break;
            case SheetKind::EmissionVectorBlank:
                extractEmissionVector(sheet, axes.emRows, "Blank", record);
                break;
            case SheetKind::AbsSample:
                extractAbsSpectrum(sheet, axes.exRows, "Sample", record);
                break;
            case SheetKind::AbsBlank:
                extractAbsSpectrum(sheet, axes.exRows, "Blank", record);
                break;
            case SheetKind::Unknown:
                break;
        }
    }

    return record;
}

// ---------------------------------------------------------------------
// Grouping samples by measurement signature, and writing a group of
// samples that share one (see the top-of-file overview for why: samples
// with identical excitation/emission/XCorrect/MCorrect write those once,
// not once per sample).
// ---------------------------------------------------------------------

// Exact-match key over a sample's excitation + emission + XCorrect +
// MCorrect values, plus park_wavelength_nm/ccd_gain_factor/ccd_xbin/
// ccd_adc_readout/ccd_gain - any difference in any of these means the
// samples don't genuinely share one measurement configuration and belong
// in different buckets/groups. Numeric fields are the raw bytes,
// length-prefixed per vector so a longer array can never coincidentally
// collide with a shorter one that shares the same leading bytes; string
// fields are length-prefixed too, for the same reason. Deliberately
// exact, not a numeric tolerance: these values are read directly from
// stored data, not recomputed, so the same instrument configuration
// should reproduce identically.
std::string measurementSignatureKey(const SampleRecord& record)
{
    std::string key;
    auto appendVec = [&](const std::vector<double>& v) {
        size_t n = v.size();
        key.append(reinterpret_cast<const char*>(&n), sizeof(n));
        if (n > 0)
            key.append(reinterpret_cast<const char*>(v.data()), n * sizeof(double));
    };
    auto appendDouble = [&](double d) { key.append(reinterpret_cast<const char*>(&d), sizeof(d)); };
    auto appendInt = [&](int i) { key.append(reinterpret_cast<const char*>(&i), sizeof(i)); };
    auto appendString = [&](const std::string& s) {
        size_t n = s.size();
        key.append(reinterpret_cast<const char*>(&n), sizeof(n));
        key.append(s);
    };
    appendVec(record.excitation);
    appendVec(record.emission);
    appendVec(record.xCorrect);
    appendVec(record.mCorrect);
    appendDouble(record.parkWavelengthNm);
    appendDouble(record.ccdGainFactor);
    appendInt(record.ccdXBin);
    appendString(record.ccdAdcReadout);
    appendString(record.ccdGain);
    return key;
}

// Buckets records by exact measurement signature, preserving first-seen
// order of both buckets and records within a bucket.
std::vector<std::vector<SampleRecord*>> organizeIntoBuckets(std::vector<SampleRecord>& records)
{
    std::vector<std::vector<SampleRecord*>> buckets;
    std::unordered_map<std::string, size_t> bucketIndexBySignature;

    for (SampleRecord& record : records)
    {
        std::string key = measurementSignatureKey(record);
        auto it = bucketIndexBySignature.find(key);
        if (it == bucketIndexBySignature.end())
        {
            bucketIndexBySignature.emplace(key, buckets.size());
            buckets.push_back({&record});
        }
        else
        {
            buckets[it->second].push_back(&record);
        }
    }

    return buckets;
}

// Writes a 1D double variable dimensioned only by `sampleDim`, one value
// per record in `bucket`, in bucket order. Returns the NcVar so the
// caller can attach a missing_value attribute where that convention
// applies (see kMissingValue).
NcVar writeSampleDoubleVar(NcGroup& scope, const NcDim& sampleDim, const std::string& name,
                            const std::vector<SampleRecord*>& bucket,
                            double SampleRecord::*field)
{
    NcVar var = scope.addVar(name, ncDouble, sampleDim);
    for (size_t i = 0; i < bucket.size(); i++)
    {
        double value = bucket[i]->*field;
        var.putVar({i}, {1}, &value);
    }
    return var;
}

// Writes a 1D nc_STRING variable dimensioned only by `sampleDim`, one
// value per record in `bucket`, in bucket order. Returns the NcVar so the
// caller can attach a missing_value attribute where that convention
// applies (see kMissingValue).
NcVar writeSampleStringVar(NcGroup& scope, const NcDim& sampleDim, const std::string& name,
                            const std::vector<SampleRecord*>& bucket,
                            std::string SampleRecord::*field)
{
    NcVar var = scope.addVar(name, ncString, sampleDim);
    for (size_t i = 0; i < bucket.size(); i++)
    {
        const char* value = (bucket[i]->*field).c_str();
        var.putVar({i}, {1}, &value);
    }
    return var;
}

// Writes a 1D double variable dimensioned by `dim` (excitation or
// emission), one per record's own vector, into a (dim, sample) variable -
// declared dim-first (not sample-first) for the same MATLAB-ncread reason
// as S1Sample/S1Blank (see "EEM MATRIX DIMENSION ORDER" at the top of the
// file): netCDF's C-order is reversed on read, so dim-first here becomes
// sample-first once read into MATLAB.
void writeSample1dVar(NcVar& var, size_t i, const std::vector<double>& values, size_t expectedSize)
{
    if (values.size() != expectedSize)
        return;  // left at the variable's default fill value - see nc.set_Fill() at the call site
    var.putVar({0, i}, {expectedSize, 1}, values.data());
}

// CF/ACDD descriptive attributes per variable, transcribed from
// "netcdf variable attributes.xlsx" (kept alongside this file). A null
// field means the spreadsheet said not to set that attribute for this
// variable (e.g. most variables have no applicable CF standard_name, and
// excitation/emission/the Note-derived scalars/identity strings are
// deliberately not given a "coordinates" attribute, since nothing else
// references them the way S1Sample etc. reference excitation/emission/
// data_identifier). missing_value is handled separately, alongside each
// field's own default (see kMissingValue's comment) - not part of this
// table, since it also has to match the SampleRecord field's actual
// sentinel value, not just a written attribute.
struct CfAttributes
{
    const char* standardName;
    const char* longName;
    const char* coverageContentType;
    const char* units;
    const char* coordinates;
};

const std::unordered_map<std::string, CfAttributes> kCfAttributes = {
    {"excitation", {"radiation_wavelength",
                     "Excitation wavelength in nm, coordinate variable physicalMeasurement types "
                     "fluorescence_sample fluorescence_blank",
                     "coordinate", "nanometer", nullptr}},
    {"emission", {"radiation_wavelength",
                   "Emission wavelength in nm, coordinate variable physicalMeasurement types "
                   "fluorescence_sample fluorescence_blank",
                   "coordinate", "nanometer", nullptr}},
    {"XCorrect", {nullptr, "Instrumen-specific excitation correction factors for multiplication of signal",
                   "physicalMeasurement", "1", "excitation"}},
    {"MCorrect", {nullptr, "Instrumen-specific emission correction factors for multiplication of signal",
                   "physicalMeasurement", "1", "emission"}},
    {"S1Sample", {nullptr, "Raw, entirely uncorrected fluorescence observations resulting from the excitation of the sample",
                   "physicalMeasurement", "1", "data_identifier emission excitation"}},
    {"S1Blank", {nullptr, "Raw, entirely uncorrected fluorescence observations resulting from the excitation of the blank",
                  "physicalMeasurement", "1", "data_identifier emission excitation"}},
    {"R1_Sample", {nullptr, "Intensity of the reference diode at every excitation wavelength during the sample measurement",
                    "physicalMeasurement", "1", "data_identifier excitation"}},
    {"R1_Blank", {nullptr, "Intensity of the reference diode at every excitation wavelength during the blank measurement",
                   "physicalMeasurement", "1", "data_identifier excitation"}},
    {"AbsI1_Sample", {nullptr, "Raw, entirely uncorrected diode intensity after the attenuation of light by the sample",
                       "physicalMeasurement", "1", "data_identifier excitation"}},
    {"AbsI1_Blank", {nullptr, "Raw, entirely uncorrected diode intensity after the attenuation of light by the blank",
                      "physicalMeasurement", "1", "data_identifier excitation"}},
    {"S1Dark_Sample", {nullptr, "CCD detector signal under dark conditions immediately prior to sample measurement",
                        "physicalMeasurement", "1", "data_identifier emission"}},
    {"S1Dark_Blank", {nullptr, "CCD detector signal under dark conditions immediately prior to blank measurement",
                       "physicalMeasurement", "1", "data_identifier emission"}},
    {"R1dark_Sample", {nullptr, "Intensity of the reference diode under dark conditions immediately prior to sample measurement",
                        "physicalMeasurement", "1", "data_identifier_i"}},
    {"R1dark_Blank", {nullptr, "Intensity of the reference diode under dark conditions immediately prior to blank measurement",
                       "physicalMeasurement", "1", "data_identifier_i"}},
    {"AbsI1dark_Sample", {nullptr, "Intensity of the absorbance diode under dark conditions immediately prior to sample measurement",
                           "physicalMeasurement", "1", "data_identifier_i"}},
    {"AbsI1dark_Blank", {nullptr, "Intensity of the absorbance diode under dark conditions immediately prior to blank measurement",
                          "physicalMeasurement", "1", "data_identifier_i"}},
    {"integration_time", {nullptr, "Integration time used for the measurement",
                           "auxiliaryInformation", "1", "data_identifier_i"}},
    {"park_wavelength_nm", {nullptr, "Fixed centering value of the emission detector",
                             "auxiliaryInformation", "1", "data_identifier_i"}},
    {"ccd_gain_factor", {nullptr, "Detector gain factor", "auxiliaryInformation", "1", "data_identifier_i"}},
    {"ccd_xbin", {nullptr, "Pixel binning of emission detector", "auxiliaryInformation", "1", "data_identifier_i"}},
    {"workbook_name", {nullptr, "Sample identifier as retreived from the workbook property",
                        "auxiliaryInformation", "1", "data_identifier_i"}},
    {"workbook_short_name", {nullptr, "Sample identifier as retreived from the short identifier workbook property",
                              "auxiliaryInformation", "1", "data_identifier_i"}},
    {"source_opj_file", {nullptr, "File from which the measurements were extracted",
                          "auxiliaryInformation", "1", "data_identifier_i"}},
    // data_identifier is no longer the coordinate variable - it doesn't
    // even share a name with the dimension anymore, now that the
    // dimension itself is named "data_identifier_i" (see writeBucket()'s
    // addDim() comment); data_identifier_i (below) fills that role
    // instead. data_identifier is a plain informational string, not
    // referenced in any other variable's "coordinates" attribute, since a
    // non-monotonic string can't validly serve as a CF coordinate (see
    // data_identifier_i's own comment for why it exists at all).
    {"data_identifier", {nullptr, "Data identifier as provided prior to measurement",
                          "auxiliaryInformation", "1", nullptr}},
    {"ccd_adc_readout", {nullptr, "Gain setting 1", "auxiliaryInformation", "1", "data_identifier_i"}},
    {"ccd_gain", {nullptr, "Gain setting 2", "auxiliaryInformation", "1", "data_identifier_i"}},
    {"experiment_file", {nullptr, "The XML file from which the measurement was orchestrated",
                          "auxiliaryInformation", "1", "data_identifier_i"}},
    {"creation_time", {nullptr, "Date and time at which the measurement finished.",
                        "auxiliaryInformation", "1", "data_identifier_i"}},
    {"modification_time", {nullptr, "Date and time at which the data processing finished.",
                            "auxiliaryInformation", "1", "data_identifier_i"}},
    // The actual coordinate variable for the "data_identifier_i" dimension
    // (the dimension itself is named after this variable, not after the
    // data_identifier string - see writeBucket()'s addDim() comment) - a
    // plain monotonically increasing integer index (1..N within each
    // bucket), added because several compliance checkers (CF-checker in
    // particular - see the compliance investigation) require a
    // dimension's coordinate variable to hold monotonic values, which a
    // string identifier that's deliberately allowed to repeat (see
    // "SAMPLE IDENTITY" above) can never satisfy. data_identifier (the
    // string) is kept as-is for human/provenance identification; this is
    // purely the machine-facing, always-valid axis coordinate.
    {"data_identifier_i", {nullptr,
                            "Numeric, monotonically increasing data identifier, only unique within the dataset",
                            "coordinate", "1", nullptr}},
};

void applyCfAttributes(NcVar var, const std::string& variableName)
{
    auto it = kCfAttributes.find(variableName);
    if (it == kCfAttributes.end())
        return;
    const CfAttributes& a = it->second;
    if (a.standardName)
        var.putAtt("standard_name", a.standardName);
    if (a.longName)
        var.putAtt("long_name", a.longName);
    if (a.coverageContentType)
        var.putAtt("coverage_content_type", a.coverageContentType);
    if (a.units)
        var.putAtt("units", a.units);
    if (a.coordinates)
        var.putAtt("coordinates", a.coordinates);
}

// missing_value declarations per "netcdf variable attributes.xlsx" -
// applied to every variable listed there, using whichever overload
// matches that variable's own netCDF type. Declaring this attribute
// doesn't require -9999/-9999.0/"-9999" to actually appear in a given
// variable's data - most physicalMeasurement variables here still use
// NaN for their own, pre-existing missing-data cases (a whole sheet
// absent, unrelated to Note extraction - see each extractor's comment);
// this is metadata documenting the convention, per CF/ACDD, not a
// rewrite of how missing spectral data is represented.
void applyMissingValueDouble(NcVar var) { var.putAtt("missing_value", ncDouble, kMissingValue); }
void applyMissingValueInt(NcVar var) { var.putAtt("missing_value", ncInt, kMissingValueInt); }
void applyMissingValueString(NcVar var) { var.putAtt("missing_value", kMissingValueString); }

// Writes one bucket of samples (all sharing the same excitation/emission/
// XCorrect/MCorrect) into `scope`, which is either the top-level NcFile
// (only one bucket in the whole export) or one NcGroup per bucket (more
// than one).
void writeBucket(NcGroup scope, const std::vector<SampleRecord*>& bucket)
{
    const SampleRecord& first = *bucket.front();
    size_t exCount = first.excitation.size();
    size_t emCount = first.emission.size();

    // Named "data_identifier_i", not "sample" or "data_identifier":
    // data_identifier_i is this dimension's actual CF coordinate variable
    // (see kCfAttributes) - a plain monotonically increasing integer
    // index, sharing the dimension's name per the usual CF/netCDF
    // convention. It exists specifically because the human-readable
    // "data_identifier" string can't fill that role: CF compliance
    // checkers identify a dimension's coordinate variable purely by name
    // match, and would then require it to hold strictly monotonic
    // values - which data_identifier is deliberately allowed to violate
    // (see "SAMPLE IDENTITY" above, on repeat measurements sharing an
    // identifier). Keeping the dimension itself named after the string
    // would have left it wrongly holding that role no matter what
    // attributes were set on it.
    NcDim sampleDim = scope.addDim("data_identifier_i", bucket.size());
    NcDim exDim = scope.addDim("excitation", exCount);
    NcDim emDim = scope.addDim("emission", emCount);

    NcVar exVar = scope.addVar("excitation", ncDouble, exDim);
    applyCfAttributes(exVar, "excitation");
    applyMissingValueDouble(exVar);
    exVar.putVar(first.excitation.data());

    NcVar emVar = scope.addVar("emission", ncDouble, emDim);
    applyCfAttributes(emVar, "emission");
    applyMissingValueDouble(emVar);
    emVar.putVar(first.emission.data());

    if (!first.xCorrect.empty())
    {
        NcVar xVar = scope.addVar("XCorrect", ncDouble, exDim);
        applyCfAttributes(xVar, "XCorrect");
        applyMissingValueDouble(xVar);
        xVar.putVar(first.xCorrect.data());
    }
    if (!first.mCorrect.empty())
    {
        NcVar mVar = scope.addVar("MCorrect", ncDouble, emDim);
        applyCfAttributes(mVar, "MCorrect");
        applyMissingValueDouble(mVar);
        mVar.putVar(first.mCorrect.data());
    }

    NcVar s1Sample = scope.addVar("S1Sample", ncDouble, {exDim, emDim, sampleDim});
    applyCfAttributes(s1Sample, "S1Sample");
    applyMissingValueDouble(s1Sample);
    NcVar s1Blank = scope.addVar("S1Blank", ncDouble, {exDim, emDim, sampleDim});
    applyCfAttributes(s1Blank, "S1Blank");
    applyMissingValueDouble(s1Blank);
    NcVar r1Sample = scope.addVar("R1_Sample", ncDouble, {exDim, sampleDim});
    applyCfAttributes(r1Sample, "R1_Sample");
    applyMissingValueDouble(r1Sample);
    NcVar r1Blank = scope.addVar("R1_Blank", ncDouble, {exDim, sampleDim});
    applyCfAttributes(r1Blank, "R1_Blank");
    applyMissingValueDouble(r1Blank);
    NcVar absSample = scope.addVar("AbsI1_Sample", ncDouble, {exDim, sampleDim});
    applyCfAttributes(absSample, "AbsI1_Sample");
    applyMissingValueDouble(absSample);
    NcVar absBlank = scope.addVar("AbsI1_Blank", ncDouble, {exDim, sampleDim});
    applyCfAttributes(absBlank, "AbsI1_Blank");
    applyMissingValueDouble(absBlank);
    NcVar s1DarkSample = scope.addVar("S1Dark_Sample", ncDouble, {emDim, sampleDim});
    applyCfAttributes(s1DarkSample, "S1Dark_Sample");
    applyMissingValueDouble(s1DarkSample);
    NcVar s1DarkBlank = scope.addVar("S1Dark_Blank", ncDouble, {emDim, sampleDim});
    applyCfAttributes(s1DarkBlank, "S1Dark_Blank");
    applyMissingValueDouble(s1DarkBlank);

    for (size_t i = 0; i < bucket.size(); i++)
    {
        const SampleRecord& r = *bucket[i];
        if (r.S1Sample.size() == exCount * emCount)
            s1Sample.putVar({0, 0, i}, {exCount, emCount, 1}, r.S1Sample.data());
        if (r.S1Blank.size() == exCount * emCount)
            s1Blank.putVar({0, 0, i}, {exCount, emCount, 1}, r.S1Blank.data());
        writeSample1dVar(r1Sample, i, r.R1_Sample, exCount);
        writeSample1dVar(r1Blank, i, r.R1_Blank, exCount);
        writeSample1dVar(absSample, i, r.AbsI1_Sample, exCount);
        writeSample1dVar(absBlank, i, r.AbsI1_Blank, exCount);
        writeSample1dVar(s1DarkSample, i, r.S1Dark_Sample, emCount);
        writeSample1dVar(s1DarkBlank, i, r.S1Dark_Blank, emCount);
    }

    NcVar r1DarkSampleVar = writeSampleDoubleVar(scope, sampleDim, "R1dark_Sample", bucket, &SampleRecord::R1dark_Sample);
    applyCfAttributes(r1DarkSampleVar, "R1dark_Sample");
    applyMissingValueDouble(r1DarkSampleVar);

    NcVar r1DarkBlankVar = writeSampleDoubleVar(scope, sampleDim, "R1dark_Blank", bucket, &SampleRecord::R1dark_Blank);
    applyCfAttributes(r1DarkBlankVar, "R1dark_Blank");
    applyMissingValueDouble(r1DarkBlankVar);

    NcVar absI1DarkSampleVar =
        writeSampleDoubleVar(scope, sampleDim, "AbsI1dark_Sample", bucket, &SampleRecord::AbsI1dark_Sample);
    applyCfAttributes(absI1DarkSampleVar, "AbsI1dark_Sample");
    applyMissingValueDouble(absI1DarkSampleVar);

    NcVar absI1DarkBlankVar =
        writeSampleDoubleVar(scope, sampleDim, "AbsI1dark_Blank", bucket, &SampleRecord::AbsI1dark_Blank);
    applyCfAttributes(absI1DarkBlankVar, "AbsI1dark_Blank");
    applyMissingValueDouble(absI1DarkBlankVar);

    // These four (plus data_identifier/ccd_adc_readout/ccd_gain below and
    // ccd_xbin further down) come from the compressed Note report, not a
    // spreadsheet - see kMissingValue's comment for why -9999/-9999.0
    // actually appears in their data (not just declared as an attribute)
    // whenever the Note extraction failed.
    NcVar integrationTimeVar =
        writeSampleDoubleVar(scope, sampleDim, "integration_time", bucket, &SampleRecord::integrationTime);
    applyCfAttributes(integrationTimeVar, "integration_time");
    applyMissingValueDouble(integrationTimeVar);

    NcVar parkVar = writeSampleDoubleVar(scope, sampleDim, "park_wavelength_nm", bucket, &SampleRecord::parkWavelengthNm);
    applyCfAttributes(parkVar, "park_wavelength_nm");
    applyMissingValueDouble(parkVar);

    NcVar gainFactorVar = writeSampleDoubleVar(scope, sampleDim, "ccd_gain_factor", bucket, &SampleRecord::ccdGainFactor);
    applyCfAttributes(gainFactorVar, "ccd_gain_factor");
    applyMissingValueDouble(gainFactorVar);

    NcVar xbinVar = scope.addVar("ccd_xbin", ncInt, sampleDim);
    applyCfAttributes(xbinVar, "ccd_xbin");
    applyMissingValueInt(xbinVar);
    for (size_t i = 0; i < bucket.size(); i++)
    {
        int value = bucket[i]->ccdXBin;
        xbinVar.putVar({i}, {1}, &value);
    }

    NcVar workbookNameVar = writeSampleStringVar(scope, sampleDim, "workbook_name", bucket, &SampleRecord::workbookName);
    applyCfAttributes(workbookNameVar, "workbook_name");
    applyMissingValueString(workbookNameVar);

    NcVar workbookShortNameVar =
        writeSampleStringVar(scope, sampleDim, "workbook_short_name", bucket, &SampleRecord::workbookShortName);
    applyCfAttributes(workbookShortNameVar, "workbook_short_name");
    applyMissingValueString(workbookShortNameVar);

    NcVar sourceOpjFileVar = writeSampleStringVar(scope, sampleDim, "source_opj_file", bucket, &SampleRecord::sourceOpjFile);
    applyCfAttributes(sourceOpjFileVar, "source_opj_file");
    applyMissingValueString(sourceOpjFileVar);

    NcVar dataIdentifierVar = writeSampleStringVar(scope, sampleDim, "data_identifier", bucket, &SampleRecord::dataIdentifier);
    applyCfAttributes(dataIdentifierVar, "data_identifier");
    applyMissingValueString(dataIdentifierVar);

    // 1-based, monotonically increasing within this bucket - matches
    // drEEM's own convention for a plain sample index (DS.i=(1:nSample)').
    // See kCfAttributes's own comment for why this variable exists.
    NcVar dataIdentifierIVar = scope.addVar("data_identifier_i", ncInt, sampleDim);
    applyCfAttributes(dataIdentifierIVar, "data_identifier_i");
    applyMissingValueInt(dataIdentifierIVar);
    for (size_t i = 0; i < bucket.size(); i++)
    {
        int value = static_cast<int>(i) + 1;
        dataIdentifierIVar.putVar({i}, {1}, &value);
    }

    NcVar adcReadoutVar = writeSampleStringVar(scope, sampleDim, "ccd_adc_readout", bucket, &SampleRecord::ccdAdcReadout);
    applyCfAttributes(adcReadoutVar, "ccd_adc_readout");
    applyMissingValueString(adcReadoutVar);

    NcVar ccdGainVar = writeSampleStringVar(scope, sampleDim, "ccd_gain", bucket, &SampleRecord::ccdGain);
    applyCfAttributes(ccdGainVar, "ccd_gain");
    applyMissingValueString(ccdGainVar);

    NcVar experimentFileVar = writeSampleStringVar(scope, sampleDim, "experiment_file", bucket, &SampleRecord::experimentFile);
    applyCfAttributes(experimentFileVar, "experiment_file");
    applyMissingValueString(experimentFileVar);

    NcVar creationTimeVar = writeSampleStringVar(scope, sampleDim, "creation_time", bucket, &SampleRecord::creationTime);
    applyCfAttributes(creationTimeVar, "creation_time");
    applyMissingValueString(creationTimeVar);

    NcVar modificationTimeVar =
        writeSampleStringVar(scope, sampleDim, "modification_time", bucket, &SampleRecord::modificationTime);
    applyCfAttributes(modificationTimeVar, "modification_time");
    applyMissingValueString(modificationTimeVar);
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
        // Phase 1: collect every exported sample from every input file
        // into memory, in full, before anything is written to NetCDF -
        // required because which samples share a measurement signature
        // (and therefore how many NcGroups the output needs, if any)
        // can't be known until every sample's axes/corrections are in
        // hand. See organizeIntoBuckets()/writeBucket() below.
        std::vector<SampleRecord> allSamples;
        unsigned int skippedBlankCount = 0;
        unsigned int skippedInvalidCount = 0;
        unsigned int skippedWrongTypeCount = 0;

        // Latest modification_time across every exported sample - tracked
        // as the raw time_t (not the already-formatted string) so the
        // comparison is a plain integer comparison, unaffected by any
        // timezone-offset differences between samples recorded in
        // different DST seasons (see formatTimestampLocal()'s comment).
        // Feeds the "date_modified"/"date_metadata_modified" global
        // attributes below, per the spreadsheet's instruction.
        time_t maxModificationTime = 0;

        // Earliest/latest creation_time (i.e. when a measurement itself
        // finished, not when it was later post-processed) across every
        // exported sample - feeds "time_coverage_start"/"_end"/
        // "_duration" below, same raw-time_t tracking rationale as above.
        time_t minCreationTime = 0;
        time_t maxCreationTime = 0;

        // Full audit trail of this run - every file read, every accepted
        // sample, every skip/failure, the grouping decision, and the
        // final summary - kept out of the console (see kept quiet on
        // purpose above, for the per-file/per-sample lines) but written
        // in full into the output file's "history" global attribute (per
        // the NetCDF Users Guide convention CF §2.6.2/ACDD both point
        // to). See logDiagnostic()/logInvocation() and the "history"
        // attribute written near the end of this function.
        std::vector<DiagnosticEntry> diagnosticLog;
        logInvocation(diagnosticLog, argc, argv);

        for (const auto& opjPath : opjFiles)
        {
            std::string opjPathStr = opjPath.string();
            logDiagnostic(diagnosticLog, "reading " + opjPathStr);
            // .ogw holds exactly one workbook, so the "Exporting sample: "
            // line below already identifies it - an extra "Reading ..."
            // console line ahead of it is just noise when there are many
            // .ogw files (one per sample) rather than a handful of
            // multi-sample .opj projects. (Still logged above regardless
            // of extension - only the console print is suppressed.)
            if (lowerExtension(opjPath) != ".ogw")
                std::cout << "Reading " << opjPathStr << "\n";

            OriginFile opj(opjPathStr);
            if (!opj.parse())
            {
                logDiagnostic(diagnosticLog, opjPathStr + ": could not parse file - skipped");
                continue;
            }

            // Decoded once per file (not per workbook) and matched to a
            // workbook below by structural byte-range containment - see
            // findCompressedNoteForWindow().
            std::vector<CompressedNoteCandidate> compressedNotes =
                decodeCompressedNotes(readRawFileBytes(opjPathStr));

            for (unsigned int i = 0; i < opj.excelCount(); i++)
            {
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
                    logDiagnostic(diagnosticLog, opjPathStr + ": '" + sampleId + "' skipped - Experiment Type is '" +
                                             (expSummary.expType.empty() ? "(none found)" : expSummary.expType) +
                                             "', not '" + kRequiredExperimentType + "'");
                    skippedWrongTypeCount++;
                    continue;
                }

                ValidationResult validation = validateWorkbookAxes(book);
                if (!validation.ok)
                {
                    std::string reasons;
                    for (size_t r = 0; r < validation.reasons.size(); r++)
                    {
                        if (r > 0)
                            reasons += "; ";
                        reasons += validation.reasons[r];
                    }
                    if (validation.isBlank)
                    {
                        logDiagnostic(diagnosticLog, opjPathStr + ": '" + sampleId +
                                                 "' skipped - no S1Sample sheet; this is a blank, not a sample");
                        skippedBlankCount++;
                    }
                    else
                    {
                        logDiagnostic(diagnosticLog, opjPathStr + ": '" + sampleId +
                                                 "' skipped - failed consistency checks: " + reasons);
                        skippedInvalidCount++;
                    }
                    continue;
                }

                // Find this workbook's own Note by structural containment
                // (see findCompressedNoteForWindow()) - guaranteed EM1 or
                // null, never the absorbance-only report, so no separate
                // "matched but not EEM" case to handle here. Used purely
                // as a source of report-derived scalar fields below - not
                // for identity, which comes exclusively from the
                // workbook's own label (its LongName in Origin's own
                // terms - see extractDataIdentifier()), honoring whatever
                // the user has it set to right now, including any rename
                // made after the fact in Origin's Project Explorer.
                const CompressedNoteCandidate* match =
                    findCompressedNoteForWindow(compressedNotes, book.dataStartOffset, book.dataEndOffset);

                SampleRecord record = extractWorkbook(book);
                record.workbookName = sampleId;
                record.workbookShortName = book.name;
                record.sourceOpjFile = opjPathStr;
                record.dataIdentifier = extractDataIdentifier(fullLabel);

                // Fields pulled from the compressed "GENERAL PARAMETERS:"
                // report - integration time, Park wavelength (EM1's fixed
                // park position), the CCD's X binning factor, ADC readout
                // rate, and gain - each filled in only if the matching
                // compressed note decoded far enough to reach it intact;
                // see the "Compressed Note text" section above.
                if (match)
                {
                    std::string integrationTimeText = extractIntegrationTimeText(match->decodedText);
                    if (!integrationTimeText.empty())
                    {
                        try
                        {
                            record.integrationTime = std::stod(integrationTimeText);
                        }
                        catch (const std::exception&)
                        {
                        }
                    }

                    std::string parkText = extractParkWavelengthText(match->decodedText);
                    if (!parkText.empty())
                    {
                        try
                        {
                            record.parkWavelengthNm = std::stod(parkText);
                        }
                        catch (const std::exception&)
                        {
                        }
                    }

                    std::string xBinText = extractXBinText(match->decodedText);
                    if (!xBinText.empty())
                    {
                        try
                        {
                            record.ccdXBin = std::stoi(xBinText);
                        }
                        catch (const std::exception&)
                        {
                        }
                    }

                    std::string adcText = extractLineAfterMarker(match->decodedText, "ADC: ");
                    if (!adcText.empty())
                        record.ccdAdcReadout = adcText;

                    std::string gainText = extractLineAfterMarker(match->decodedText, "Gain: ");
                    if (!gainText.empty())
                        record.ccdGain = gainText;

                    if (!adcText.empty() && !gainText.empty())
                        record.ccdGainFactor = ccdGainFactorFromReport(adcText, gainText);
                }

                std::string created = formatTimestampLocal(book.creationDate);
                if (!created.empty())
                {
                    record.creationTime = created;
                    if (minCreationTime == 0 || book.creationDate < minCreationTime)
                        minCreationTime = book.creationDate;
                    if (book.creationDate > maxCreationTime)
                        maxCreationTime = book.creationDate;
                }

                std::string modified = formatTimestampLocal(book.modificationDate);
                if (!modified.empty())
                    record.modificationTime = modified;
                if (book.modificationDate > maxModificationTime)
                    maxModificationTime = book.modificationDate;

                // expSummary.expType was already used above to filter this
                // sample in; expFilename is the other <ExpSummary> field
                // worth keeping (see extractExpSummaryFields() above) -
                // integration_time used to come from here too, but now
                // comes from the compressed report instead (see above),
                // which doesn't have this XML tag's floating-point
                // representation noise.
                if (!expSummary.expFilename.empty())
                    record.experimentFile = expSummary.expFilename;

                // A fact about the sample itself, not an action this
                // program took - so it's dated (and sorted, see the
                // "history" sort site) by the measurement's own
                // completion time (book.creationDate), not "now" like
                // every other history line. Measurement times are
                // normally years before any given run of this program,
                // so sorting the whole log chronologically puts every
                // one of these first, ahead of anything this run itself
                // did - which is the point: it's the more fundamental
                // fact.
                std::string completedAt = formatTimestampLocal(book.creationDate);
                if (!completedAt.empty())
                {
                    diagnosticLog.push_back(
                        {book.creationDate, completedAt + ": " + record.dataIdentifier + " completed measurement"});
                }

                logDiagnostic(diagnosticLog,
                              "accepted sample '" + record.dataIdentifier + "' from " + opjPathStr);
                allSamples.push_back(std::move(record));
            }
        }

        // Two different .opj files can each contain a workbook with the
        // same label/LongName - Origin's own "(NN)" auto-numbering only
        // disambiguates duplicates *within* one project, so it has no way
        // to catch (or warn about) the same identifier being reused in a
        // separate file. That's different from the same identifier
        // appearing twice *within* one file (e.g. "AO22268 (01)" and
        // "AO22268 (02)"), which is Origin's own, already-handled record
        // of a genuine, intentional repeat measurement in one session.
        // So: tag every occurrence of a data_identifier that comes from a
        // file other than the first file that used it. This is a
        // FAIR-findability flag, not a correctness problem - the samples
        // are still fully exported with their own real values, unique
        // position along the data_identifier_i dimension, and everything
        // else intact; the tag just tells a downstream reader "this
        // identifier isn't unique across the dataset" so they can look
        // closer if they want to.
        {
            std::unordered_map<std::string, std::string> firstFileForIdentifier;
            for (SampleRecord& record : allSamples)
            {
                auto [it, inserted] = firstFileForIdentifier.try_emplace(record.dataIdentifier, record.sourceOpjFile);
                if (!inserted && it->second != record.sourceOpjFile)
                    record.dataIdentifier += "_duplicate";
            }
        }

        // Phase 2: group the collected samples by exact measurement
        // signature (excitation/emission/XCorrect/MCorrect/
        // park_wavelength_nm/ccd_gain_factor/ccd_xbin/ccd_adc_readout/
        // ccd_gain - see measurementSignatureKey()), largest group first
        // (stable, so buckets of equal size keep their discovery order).
        std::vector<std::vector<SampleRecord*>> buckets = organizeIntoBuckets(allSamples);
        std::stable_sort(buckets.begin(), buckets.end(),
                          [](const auto& a, const auto& b) { return a.size() > b.size(); });

        logDiagnostic(diagnosticLog,
                       "grouping check: compared excitation, emission, XCorrect, MCorrect, "
                       "park_wavelength_nm, ccd_gain_factor, ccd_xbin, ccd_adc_readout, ccd_gain "
                       "(exact match required) across all " + std::to_string(allSamples.size()) +
                       " accepted sample(s)");
        if (buckets.size() <= 1)
        {
            logDiagnostic(diagnosticLog, "grouping result: all accepted samples share the same "
                                          "measurement configuration - no groups created");
        }
        else
        {
            std::string sizes;
            for (size_t b = 0; b < buckets.size(); b++)
            {
                if (b > 0)
                    sizes += ", ";
                sizes += std::to_string(buckets[b].size());
            }
            logDiagnostic(diagnosticLog,
                           "grouping result: " + std::to_string(buckets.size()) +
                           " distinct measurement configuration(s) found - samples grouped into " +
                           std::to_string(buckets.size()) + " measurement_type_N group(s) (sizes: " +
                           sizes + ")");
        }

        // Phase 3: write. A single bucket means every sample shares one
        // measurement configuration, so the output has no groups at all -
        // everything goes straight into the file's root. More than one
        // bucket means the input spans genuinely different configurations,
        // so each gets its own group.
        NcFile nc(argv[2], NcFile::replace);
        nc.putAtt("Conventions", "CF-1.13, ACDD-1.3");

        // ACDD discovery/provenance attributes, per "netcdf variable
        // attributes.xlsx" 's "Global attributes" sheet.
        nc.putAtt("keywords", "FDOM, CDOM, fluorescence, absorbance, excitation-emission matrix, Horiba, Aqualog");
        nc.putAtt("summary", "Rawdata exported from proprietary OPJ or OGW files created by HORIBA Aqualog spectrofluorometers");
        nc.putAtt("title", "UV-Vis absorbance and fluorescence spectra");
        nc.putAtt("instrument", "Horiba Aqualog");
        nc.putAtt("instrument_vocabulary", "NERC Vocabulary Server L22");
        nc.putAtt("standard_name_vocabulary", "CF Standard Name Table v94");
        nc.putAtt("source", "physicalMeasurement");
        nc.putAtt("cdm_data_type", "grid");
        nc.putAtt("comment", "This file was produced using the C++ tool aqualog2nc: "
                             "https://github.com/urbanwuensch/aqualog2nc in the version specified by the "
                             "attribute version_aqualog2nc. DOI: 10.5281/zenodo.21961575");
        nc.putAtt("version_aqualog2nc", "v1.0.2");

        // date_created/date_issued: both this export's own wall-clock
        // time - genuinely known (unlike a sample's own creation/
        // modification time, this one really is "right now, on this
        // machine"), so no offset ambiguity here at all. Same instant
        // for both, captured once.
        std::string exportTime = formatTimestampLocal(std::time(nullptr));
        nc.putAtt("date_created", exportTime);
        nc.putAtt("date_issued", exportTime);

        // date_modified/date_metadata_modified: per the spreadsheet, both
        // use the latest modification_time seen across every exported
        // sample (maxModificationTime, tracked as a raw time_t during
        // Phase 1 - see its own comment for why not the formatted
        // strings). Left unset if no sample had a valid modification
        // date at all.
        if (maxModificationTime > 0)
        {
            std::string latestModified = formatTimestampLocal(maxModificationTime);
            nc.putAtt("date_modified", latestModified);
            nc.putAtt("date_metadata_modified", latestModified);
        }

        // time_coverage_start/_end: earliest/latest creation_time (i.e.
        // when a measurement itself finished) across every exported
        // sample - minCreationTime/maxCreationTime, tracked the same way
        // as maxModificationTime above. _duration is the plain elapsed
        // time between the two, not a calendar-based duration (see
        // formatIsoDuration()'s own comment). All three left unset if no
        // sample had a valid creation date at all.
        if (minCreationTime > 0 && maxCreationTime > 0)
        {
            nc.putAtt("time_coverage_start", formatTimestampLocal(minCreationTime));
            nc.putAtt("time_coverage_end", formatTimestampLocal(maxCreationTime));
            nc.putAtt("time_coverage_duration", formatIsoDuration(maxCreationTime - minCreationTime));
        }

        // Every variable in this program is fully populated immediately
        // after creation, so netCDF's default "pre-fill with a fill value,
        // then overwrite" behavior is pure redundant I/O here - safe to
        // disable unconditionally.
        int oldFillMode;
        nc.set_Fill(NC_NOFILL, &oldFillMode);

        if (buckets.size() <= 1)
        {
            if (!buckets.empty())
                writeBucket(nc, buckets[0]);
        }
        else
        {
            // Zero-padded to as many digits as the largest group number
            // needs, so names still sort correctly as plain strings
            // (measurement_type_02 before measurement_type_10).
            size_t digits = std::to_string(buckets.size()).size();
            for (size_t b = 0; b < buckets.size(); b++)
            {
                std::string number = std::to_string(b + 1);
                number.insert(0, digits - std::min(digits, number.size()), '0');
                NcGroup group = nc.addGroup("measurement_type_" + number);
                writeBucket(group, buckets[b]);
            }
        }

        std::string outputPath = fs::absolute(fs::path(argv[2])).string();

        logDiagnostic(diagnosticLog,
                       "wrote " + std::to_string(allSamples.size()) + " sample(s) across " +
                       std::to_string(buckets.size()) + " measurement type(s) to " + outputPath);
        if (skippedBlankCount > 0 || skippedInvalidCount > 0 || skippedWrongTypeCount > 0)
        {
            logDiagnostic(diagnosticLog,
                           "finished (" + std::to_string(skippedBlankCount) + " blank(s) skipped, " +
                           std::to_string(skippedInvalidCount) + " sample(s) failed consistency checks, " +
                           std::to_string(skippedWrongTypeCount) + " sample(s) skipped for wrong Experiment Type)");
        }
        else
        {
            logDiagnostic(diagnosticLog, "finished");
        }

        // Full audit trail of this run - invocation, every file read,
        // every accepted sample, every skip/failure, the grouping
        // decision, and this final summary - one timestamped line per
        // action, per the NetCDF Users Guide "history" convention (see
        // logDiagnostic()/logInvocation() above). Written after all data
        // is already in the file, so it can include this run's own
        // outcome, not just what led up to it.
        //
        // Sorted chronologically by each entry's own instant (sortKey),
        // not left in collection order - this is what puts every
        // "completed measurement" entry (dated by the actual measurement
        // time, typically years earlier) ahead of this run's own
        // processing entries (all dated "now"). Stable, so entries that
        // share an instant (e.g. two files read within the same second)
        // keep their original relative order.
        {
            std::stable_sort(diagnosticLog.begin(), diagnosticLog.end(),
                              [](const DiagnosticEntry& a, const DiagnosticEntry& b) {
                                  return a.sortKey < b.sortKey;
                              });
            std::string history;
            for (size_t n = 0; n < diagnosticLog.size(); n++)
            {
                if (n > 0)
                    history += "\n";
                history += diagnosticLog[n].line;
            }
            nc.putAtt("history", history);
        }

        std::cout << "Wrote " << allSamples.size() << " sample(s) across " << buckets.size()
                  << " measurement type(s) to " << outputPath << "\n";

        if (skippedBlankCount > 0 || skippedInvalidCount > 0 || skippedWrongTypeCount > 0)
        {
            std::cout << "Done (" << skippedBlankCount << " blank(s) skipped, "
                      << skippedInvalidCount << " sample(s) failed consistency checks, "
                      << skippedWrongTypeCount << " sample(s) skipped for wrong Experiment Type - "
                      << "see the output file's \"history\" attribute for per-sample details)\n";
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
