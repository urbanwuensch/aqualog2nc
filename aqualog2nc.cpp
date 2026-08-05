// aqualog2nc.cpp
//
// Exports Aqualog EEM/absorbance data from one .opj file, or every .opj
// file found (recursively) under a folder, into a single NetCDF file -
// without needing Origin Pro installed.
//
// Usage: aqualog2nc <input.opj | input_folder> output.nc
//
// Every workbook in every input .opj becomes its own top-level NetCDF
// group, each with its own independent emission/excitation axes AND its
// own XCorrect/Mcorrect variables - there is no assumption that different
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
// source, matching aqualogimport.m's Xout.XCorrect). Mcorrect has only
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
//   5) S1DarkandMcorrectSample/Blank row count == emission axis length
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
//   - S1DarkandMcorrectSample/Blank's column 0 is an emission wavelength
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

// Recursively collects every .opj file under `input` (or just `input`
// itself, if it's already a single .opj file), sorted for a deterministic
// processing order.
std::vector<fs::path> collectOpjFiles(const fs::path& input)
{
    std::vector<fs::path> files;

    auto isOpj = [](const fs::path& p) {
        std::string ext = p.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                        [](unsigned char c) { return std::tolower(c); });
        return ext == ".opj";
    };

    if (fs::is_regular_file(input))
    {
        if (isOpj(input))
            files.push_back(input);
        return files;
    }

    if (fs::is_directory(input))
    {
        for (const auto& entry : fs::recursive_directory_iterator(input))
        {
            if (entry.is_regular_file() && isOpj(entry.path()))
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

    // Check 5: S1DarkandMcorrect sheets (Mcorrect's source) - row count
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
    EmissionVectorSample,    // S1DarkandMcorrectSample - S1Dark
    EmissionVectorBlank,     // S1DarkandMcorrectBlank  - S1Dark, Mcorrect
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

// S1DarkandMcorrectSample / S1DarkandMcorrectBlank: one row per emission
// wavelength, no reversal. Mcorrect has only ever had one source sheet
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
        std::vector<double> mcorrect = gatherValues(sheet.columns[2], emRows);
        NcVar mVar = group.addVar("Mcorrect", ncDouble, emDim);
        mVar.putVar(mcorrect.data());
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
        std::cout << "Usage: aqualog2nc <input.opj | input_folder> output.nc\n";
        return 1;
    }

    fs::path inputPath(argv[1]);
    std::vector<fs::path> opjFiles = collectOpjFiles(inputPath);

    if (opjFiles.empty())
    {
        std::cerr << "No .opj files found at '" << argv[1] << "'\n";
        return 1;
    }

    std::cout << "Found " << opjFiles.size() << " .opj file(s)\n";

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
        //unsigned int sampleIndex = 0;

        for (const auto& opjPath : opjFiles)
        {
            std::string opjPathStr = opjPath.string();
            std::cout << "Reading " << opjPathStr << "\n";

            OriginFile opj(opjPathStr);
            if (!opj.parse())
            {
                std::cerr << "  [error] could not parse '" << opjPathStr << "' - skipped\n";
                continue;
            }

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

                std::string created = formatTimestampUtc(book.creationDate);
                if (!created.empty())
                    group.putAtt("creation_time", created);

                std::string modified = formatTimestampUtc(book.modificationDate);
                if (!modified.empty())
                    group.putAtt("modification_time", modified);

                exportWorkbook(group, book);

                //auto sampleEnd = std::chrono::steady_clock::now();
                //auto elapsedMs =
                //    std::chrono::duration_cast<std::chrono::milliseconds>(sampleEnd - sampleStart).count();
                //sampleIndex++;
                //std::cout << "    (sample #" << sampleIndex << ", " << elapsedMs << " ms)\n";
            }
        }

        if (skippedBlankCount > 0 || skippedInvalidCount > 0)
        {
            std::cout << "Done (" << skippedBlankCount << " blank(s) skipped, "
                      << skippedInvalidCount << " sample(s) failed consistency checks)\n";
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
