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
// first in that workbook). Which sheet won is only printed to the console,
// not stored as a file attribute.
//
// XCORRECT CONSOLIDATION
// Up to three sheets can each carry a copy of the excitation correction
// factor (R1andR1cBlank, R1andR1cSample, AbsSpectrumBlank) - these should
// all represent the same physical quantity. writeXCorrect() gathers
// whichever of these exist for a workbook, compares them, prints the
// largest discrepancy found to the console, and writes a single
// "XCorrect" variable (preferring R1andR1cBlank as the canonical source,
// matching aqualogimport.m's Xout.XCorrect). Mcorrect has only ever had
// one source, so it's unchanged, still written per-workbook on the
// emission axis.
//
// SAMPLE TIMESTAMPS
// Origin::Excel inherits Window::creationDate / Window::modificationDate
// (time_t), which Origin sets when the workbook is created/last modified.
// These are written per-workbook as "creation_time" / "modification_time"
// ISO-8601 UTC string attributes.
//
// BATCH MODE / GROUP NAME COLLISIONS
// Two different .opj files can each contain a workbook with the same name
// (e.g. both call it "Sample_1"). uniqueGroupName() appends a numeric
// suffix in that case, and every group also gets a source_opj_file
// attribute so you can always trace a sample back to its origin file.
//
// ASSUMPTIONS TO VERIFY AGAINST YOUR OriginObj.h / OriginFile.h:
//   - Origin::Variant has type() returning V_DOUBLE or V_STRING, plus
//     as_double() / as_string(). Origin::Window has time_t creationDate
//     and modificationDate. (Both confirmed against the public liborigin
//     fork's OriginObj.h - your local copy may differ slightly.)
//   - Origin::SpreadSheet::columns[i].data is a vector<Origin::Variant>.
//   - The "Note" worksheet's entire text lives in a single Variant
//     (columns[0].data[0]) as one \n-delimited string, matching
//     worksheetData{1} in aqualogimport.m.
//   - Worksheet short names may contain spaces/separators the way Origin
//     displays them (e.g. "S1 Sample"); normalizeSheetName() strips the
//     same characters aqualogimport.m does before comparing.
//   - Different sheets in the same workbook share the same raw row
//     layout/padding, so row indices computed from one sheet's axis
//     column are valid to reuse when reading another sheet's data
//     columns (including the different XCorrect source sheets). This
//     should hold for a single combined acquisition run; the diagnostic
//     printouts below will make it obvious if it doesn't.
//   - NcGroup::getGroup(name) returns a null NcGroup (isNull() == true)
//     when no child group of that name exists yet, matching the
//     getDim()/getVar() convention used elsewhere in netcdf-cxx4. Double
//     check this against your installed netcdf-cxx4 version.

#include "OriginFile.h"

#include <netcdf>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
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

std::string safeName(const std::string& input)
{
    std::string out = input;
    for (char& c : out)
    {
        if (c == ' ' || c == '/' || c == '\\' || c == ':' || c == '-' || c == '.')
            c = '_';
    }
    if (out.empty())
        out = "unnamed";
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

std::string variantToString(const Origin::Variant& v)
{
    if (v.type() == Origin::Variant::V_STRING)
        return std::string(v.as_string());
    return std::string();
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

// Appends a numeric suffix if a group of this name already exists at the
// top level of `nc` (can happen when two different .opj files each have a
// workbook with the same name).
std::string uniqueGroupName(NcFile& nc, const std::string& baseName)
{
    if (nc.getGroup(baseName).isNull())
        return baseName;

    for (int suffix = 2;; suffix++)
    {
        std::string candidate = baseName + "_" + std::to_string(suffix);
        if (nc.getGroup(candidate).isNull())
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
    Note,                    // Note - free-text metadata
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
    if (normalizedName == "Note")                    return SheetKind::Note;

    return SheetKind::Unknown;
}

// ---------------------------------------------------------------------
// Per-sheet-type writers
// ---------------------------------------------------------------------

// S1Sample / S1Blank: emission (rows) x excitation (cols) matrix.
//   dat = cell2mat(worksheetData(:,2:end));
//   Xout.S1Sample(j,:,:) = flip(dat,2);   <- reverse column order
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
                values[r * cols + c] = variantToDouble(col.data[srcRow]);
        }
    }

    NcVar intensity = group.addVar(varName, ncDouble, {emDim, exDim});
    intensity.putVar(values.data());
    intensity.putAtt("coordinates", "emission excitation");
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
// shared "excitation" axis. Column 5 (the other redundant XCorrect source
// in the Blank sheet) is handled separately by writeXCorrect().
void writeAbsSpectrum(NcGroup& group, Origin::SpreadSheet& sheet,
                       const NcDim& exDim, const std::vector<size_t>& exRows,
                       const std::string& label)
{
    if (sheet.columns.size() < 5 || exDim.isNull() || exRows.empty())
    {
        std::cerr << "  [warn] sheet '" << sheet.name
                  << "': fewer columns than expected, or missing excitation axis - skipped\n";
        return;
    }

    auto writeVar = [&](const std::string& name, size_t colIndex) {
        if (colIndex >= sheet.columns.size())
            return;
        std::vector<double> v = gatherValues(sheet.columns[colIndex], exRows);
        NcVar var = group.addVar(name, ncDouble, exDim);
        var.putVar(v.data());
    };

    writeVar("AbsI1_" + label, 1);
    writeVar("AbsR1_" + label, 3);
    group.putAtt("AbsI1dark_" + label, ncDouble,
                 sheet.columns[2].data.empty() ? kNaN : variantToDouble(sheet.columns[2].data[0]));
    group.putAtt("AbsR1dark_" + label, ncDouble,
                 sheet.columns[4].data.empty() ? kNaN : variantToDouble(sheet.columns[4].data[0]));

    if (label == "Sample" && sheet.columns.size() > 9)
        writeVar("Abs_horiba", 9);
}

// Note: free-text metadata. Stores key lines plus the full raw text.
void writeNoteMetadata(NcGroup& group, Origin::SpreadSheet& sheet)
{
    if (sheet.columns.empty() || sheet.columns[0].data.empty())
        return;

    std::string text = variantToString(sheet.columns[0].data[0]);
    if (text.empty())
        return;

    auto extractLine = [&](const std::string& prefix) -> std::string {
        size_t pos = text.find(prefix);
        if (pos == std::string::npos)
            return "";
        pos += prefix.size();
        size_t end = text.find('\n', pos);
        std::string value = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        while (!value.empty() && (value.back() == '\r' || value.back() == '\n'))
            value.pop_back();
        return value;
    };

    std::string experimentType = extractLine("Experiment Type: ");
    std::string integrationTime = extractLine("Integration Time: ");
    std::string emPark = extractLine("Park: ");

    if (!experimentType.empty())
        group.putAtt("note_experiment_type", experimentType);
    if (!integrationTime.empty())
        group.putAtt("note_integration_time", integrationTime);
    if (!emPark.empty())
        group.putAtt("note_em_park_position", emPark);

    group.putAtt("note_raw_text", text);
}

// XCorrect: up to three sheets can each carry a copy of this correction
// factor - R1andR1cBlank (column 3), R1andR1cSample (column 3, if
// present), and AbsSpectrumBlank (column 5). Gathers whichever exist,
// compares them, and writes exactly one "XCorrect" variable per workbook.
// The winning source and any discrepancy are printed to the console only.
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

    if (candidates.size() > 1)
    {
        std::cout << "    XCorrect: " << candidates.size() << " source(s) found (";
        for (size_t i = 0; i < candidates.size(); i++)
            std::cout << (i ? ", " : "") << candidates[i].source;
        std::cout << "), max discrepancy " << maxDiff << " - using " << candidates.front().source << "\n";

        if (maxDiff > kXCorrectTolerance)
        {
            std::cerr << "  [warn] workbook '" << book.name << "': XCorrect sources disagree by up to "
                      << maxDiff << "\n";
        }
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

WorkbookAxes buildAxes(NcGroup& group, Origin::Excel& book)
{
    WorkbookAxes axes;

    Origin::SpreadSheet* emSource = findSheet(book, "S1Sample");
    if (!emSource)
        emSource = findSheet(book, "S1Blank");

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

            if (axes.emRows.size() != emSource->columns[0].data.size())
            {
                std::cout << "    emission axis: " << axes.emRows.size() << " values (dropped "
                          << (emSource->columns[0].data.size() - axes.emRows.size())
                          << " out-of-range rows)\n";
            }
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

        std::cout << "    excitation axis: " << excitation.size() << " values from " << key;
        if (rows.size() != sheet->columns[0].data.size())
            std::cout << " (dropped " << (sheet->columns[0].data.size() - rows.size())
                      << " out-of-range rows)";
        std::cout << " - " << excitation.front() << " to " << excitation.back() << " nm\n";
        break;
    }

    if (!axes.exDim.isNull() && emSource)
    {
        size_t matrixCols = emSource->columns.size() - 1;
        if (matrixCols != static_cast<size_t>(axes.exDim.getSize()))
        {
            std::cerr << "  [warn] workbook '" << book.name << "': EEM matrix has "
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
                case SheetKind::Note:
                    writeNoteMetadata(group, sheet);
                    break;
                case SheetKind::Unknown:
                    std::cerr << "  [warn] unrecognized sheet '" << sheet.name
                              << "' in workbook '" << book.name << "' - skipped\n";
                    break;
            }
        }
        catch (const NcException& e)
        {
            std::cerr << "  [error] sheet '" << sheet.name << "' in workbook '" << book.name
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
                Origin::Excel& book = opj.excel(i);
                std::string baseName = safeName(book.name);
                std::string groupName = uniqueGroupName(nc, baseName);

                std::cout << "  Exporting sample: " << book.name;
                if (groupName != baseName)
                    std::cout << " (name collision - stored as '" << groupName << "')";
                std::cout << "\n";

                NcGroup group = nc.addGroup(groupName);
                group.putAtt("workbook_name", book.name);
                group.putAtt("source_opj_file", opjPathStr);

                std::string created = formatTimestampUtc(book.creationDate);
                if (!created.empty())
                    group.putAtt("creation_time", created);

                std::string modified = formatTimestampUtc(book.modificationDate);
                if (!modified.empty())
                    group.putAtt("modification_time", modified);

                exportWorkbook(group, book);
            }
        }

        std::cout << "Done\n";
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
