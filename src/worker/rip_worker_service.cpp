#include "rip_worker_service.h"

#include "rip/raw_interop_image.h"
#include "rip/prn_stream_rewriter.h"
#include "rip/white_plate_generator.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

extern "C" {

typedef unsigned long model_cap_t;

typedef struct
{
    const char* name;
    const char* text;
    unsigned short hres;
    unsigned short vres;
    unsigned short printed_hres;
    unsigned short printed_vres;
    unsigned short vertical_passes;
    stp_raw_t* command;
    stp_vars_t* v;
} res_t;

typedef struct
{
    short color;
    short subchannel;
    short head_offset;
    short split_channel_count;
    const char* channel_density;
    const char* subchannel_transition;
    const char* subchannel_value;
    const char* subchannel_scale;
    const char* name;
    const char* text;
    short* split_channels;
} physical_subchannel_t;

typedef struct
{
    const char* name;
    unsigned short n_subchannels;
    physical_subchannel_t* subchannels;
    const char* hue_curve_name;
    stp_curve_t* hue_curve;
} ink_channel_t;

typedef enum
{
    INKSET_CMYK = 0,
    INKSET_CcMmYK = 1,
    INKSET_CcMmYyK = 2,
    INKSET_CcMmYKk = 3,
    INKSET_QUADTONE = 4,
    INKSET_HEXTONE = 5,
    INKSET_OTHER = 6,
    INKSET_EXTENDED = 7
} inkset_id_t;

typedef struct
{
    const char* name;
    const char* text;
    unsigned short channel_count;
    unsigned short aux_channel_count;
    inkset_id_t inkset;
    const stp_raw_t* init_sequence;
    const stp_raw_t* deinit_sequence;
    ink_channel_t* channels;
    ink_channel_t* aux_channels;
} inkname_t;

typedef struct
{
    unsigned int n_shades;
    double* shades;
} shade_t;

typedef struct
{
    const char* name;
    const char* text;
    unsigned short n_shades;
    unsigned short n_inks;
    const stp_raw_t* init_sequence;
    const stp_raw_t* deinit_sequence;
    shade_t* shades;
    inkname_t* inknames;
} inklist_t;

typedef struct escp2_printer
{
    int active;
    model_cap_t flags;
    short nozzles;
    short min_nozzles;
    short nozzle_start;
    short nozzle_separation;
    short black_nozzles;
    short min_black_nozzles;
    short black_nozzle_start;
    short black_nozzle_separation;
    short fast_nozzles;
    short min_fast_nozzles;
    short fast_nozzle_start;
    short fast_nozzle_separation;
    short physical_channels;
    short base_separation;
} stpi_escp2_printer_t;

extern const res_t* stpi_escp2_find_resolution(const stp_vars_t* v);
extern const inklist_t* stpi_escp2_inklist(const stp_vars_t* v);
extern stpi_escp2_printer_t* stpi_escp2_get_printer(const stp_vars_t* v);

}

namespace {

void appendWorkerRuntimeLog(const std::string& line)
{
    std::error_code ec;
    const auto path = std::filesystem::current_path(ec) / "yink-worker-runtime.log";
    if (ec) {
        return;
    }
    std::ofstream out(path, std::ios::app);
    if (out) {
        out << line << '\n';
    }
}

struct OutputContext {
    std::ofstream out;
    grpc::ServerReaderWriter<gutenrip::worker::RenderPlaneStreamResponse,
                             gutenrip::worker::RenderPlaneStreamRequest>* stream = nullptr;
    yink::rip::ChannelOffsetCorrector* offsetCorrector = nullptr;
    std::size_t bytesWritten = 0;
    bool writeFailed = false;
};

struct LayoutImageContext {
    int pageWidthPx = 1;
    int pageHeightPx = 1;
    int imageX = 0;
    int imageY = 0;
    int imageWidth = 1;
    int imageHeight = 1;
    double pageWidthMM = 1.0;
    double pageHeightMM = 1.0;
    double imageXMM = 0.0;
    double imageYMM = 0.0;
    double imageWidthMM = 1.0;
    double imageHeightMM = 1.0;
    std::uint8_t fill = 0;
    bool isWhite = false;
    bool hasSourceImage = false;
    std::size_t rawChannelCount = 0;
    std::vector<std::uint16_t> rawPixels;
    int sourceWidth = 0;
    int sourceHeight = 0;
    std::string sourceInputImageType = "RGB";
    bool whiteRgbInput = false;
    bool whiteRawInput = false;
    std::size_t whiteRawChannelCount = 1;
    int whiteRawLightCyanChannel = -1;
    int whiteRawLightMagentaChannel = -1;
    grpc::ServerReaderWriter<gutenrip::worker::RenderPlaneStreamResponse,
                             gutenrip::worker::RenderPlaneStreamRequest>* stream = nullptr;
    std::string streamError;
};

std::size_t colorChannelCount(const std::string& inputImageType)
{
    if (inputImageType == "CMYK" || inputImageType == "KCMY") return 4u;
    if (inputImageType == "Grayscale" || inputImageType == "Whitescale") return 1u;
    return 3u;
}

bool envIsSet(const char* name)
{
    const char* value = std::getenv(name);
    return value && value[0] != '\0';
}

void setEnv(const char* name, const std::filesystem::path& value)
{
#ifdef _WIN32
    _putenv_s(name, value.string().c_str());
#else
    setenv(name, value.string().c_str(), 0);
#endif
}

void setEnvString(const char* name, const std::string& value)
{
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

void configureGutenprintDebugFlags()
{
    constexpr unsigned long noCompression = 0x400000;
    const char* current = std::getenv("STP_DEBUG");
    unsigned long flags = current && current[0] != '\0' ? std::strtoul(current, nullptr, 0) : 0;
    flags |= noCompression;
    setEnvString("STP_DEBUG", std::to_string(flags));
}

bool xmlDataPathContainsLocalDrivers(const std::filesystem::path& dataPath)
{
    const std::filesystem::path escp2 = dataPath / "printers" / "escp2.xml";
    if (!std::filesystem::exists(escp2)) {
        return false;
    }

    std::ifstream in(escp2, std::ios::binary);
    if (!in) {
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    return text.find("escp2-l18050") != std::string::npos &&
           text.find("escp2-et8550") != std::string::npos;
}

std::filesystem::path getExecutableDir()
{
#ifdef _WIN32
    wchar_t buffer[MAX_PATH];
    const DWORD len = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        return std::filesystem::path(buffer).parent_path();
    }
#endif
    return std::filesystem::current_path();
}

void configureGutenprintDataPath()
{
    if (envIsSet("STP_DATA_PATH") &&
        xmlDataPathContainsLocalDrivers(std::getenv("STP_DATA_PATH"))) {
        return;
    }

    const std::filesystem::path currentDir = std::filesystem::current_path();
    const std::filesystem::path executableDir = getExecutableDir();
    const std::filesystem::path candidates[] = {
        currentDir.parent_path() / "xml",
        executableDir.parent_path() / "xml",
        currentDir / "xml",
        executableDir / "xml",
        currentDir / "gutenprint" / "src" / "xml",
        currentDir.parent_path() / "gutenprint" / "src" / "xml",
        currentDir / "worker" / "gutenprint" / "src" / "xml",
        currentDir.parent_path() / "worker" / "gutenprint" / "src" / "xml",
        executableDir / "gutenprint" / "src" / "xml",
        executableDir.parent_path() / "gutenprint" / "src" / "xml",
        executableDir / "worker" / "gutenprint" / "src" / "xml",
        executableDir.parent_path() / "worker" / "gutenprint" / "src" / "xml",
    };

    for (const auto& candidate : candidates) {
        if (xmlDataPathContainsLocalDrivers(candidate)) {
            setEnv("STP_DATA_PATH", candidate);
            std::cout << "STP_DATA_PATH=" << candidate.string() << '\n';
            return;
        }
    }
}

int positiveDpi(int dpi)
{
    return dpi > 0 ? dpi : 1440;
}

std::string toLowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string normalizedPrinterModel(const std::string& label)
{
    const std::string lower = toLowerAscii(label);
    if (lower.find("et-8550") != std::string::npos) {
        return "ET-8550";
    }
    if (lower.find("l18050") != std::string::npos) {
        return "L18050";
    }
    return label;
}

bool isCatalogPrinter(const stp_printer_t* printer)
{
    if (!printer) {
        return false;
    }
    const int model = stp_printer_get_model(printer);
    const char* family = stp_printer_get_family(printer);
    return family && std::strcmp(family, "escp2") == 0 && model >= 900 && model <= 999;
}

std::string defaultCatalogDriver()
{
    const int printerCount = stp_printer_model_count();
    for (int i = 0; i < printerCount; ++i) {
        const stp_printer_t* printer = stp_get_printer_by_index(i);
        if (isCatalogPrinter(printer) && stp_printer_get_driver(printer)) {
            return stp_printer_get_driver(printer);
        }
    }
    return {};
}

int escIColorForSubchannel(const physical_subchannel_t& subchannel)
{
    const int color = static_cast<int>(subchannel.color);
    if (subchannel.subchannel < 0) {
        return color;
    }
    return color | (static_cast<int>(subchannel.subchannel) << 4);
}

int intParameterForResolution(stp_vars_t* vars,
                              const res_t* res,
                              const char* parameter)
{
    if (res && res->v &&
        stp_check_int_parameter(res->v, parameter, STP_PARAMETER_ACTIVE)) {
        return stp_get_int_parameter(res->v, parameter);
    }
    if (vars && stp_check_int_parameter(vars, parameter, STP_PARAMETER_ACTIVE)) {
        return stp_get_int_parameter(vars, parameter);
    }
    return 0;
}

void addResolutionProfile(stp_vars_t* vars,
                          const stp_param_string_t* option,
                          gutenrip::worker::PrintDriverProfile* profile)
{
    if (!vars || !option || !option->name || !profile) {
        return;
    }
    if (std::strcmp(option->name, "None") == 0) {
        return;
    }

    stp_set_string_parameter(vars, "Resolution", option->name);
    const res_t* res = stpi_escp2_find_resolution(vars);
    if (!res) {
        return;
    }

    auto* resolution = profile->add_resolutions();
    resolution->set_name(option->name);
    resolution->set_label(option->text && option->text[0] != '\0' ? option->text : option->name);
    resolution->set_physical_dpi_x(res->hres);
    resolution->set_physical_dpi_y(res->vres);
    resolution->set_printed_dpi_x(res->printed_hres);
    resolution->set_printed_dpi_y(res->printed_vres);
    resolution->set_base_dpi_x(
        intParameterForResolution(vars, res, "escp2_base_res"));
}

void fillResolutions(stp_vars_t* vars,
                     gutenrip::worker::PrintDriverProfile* profile)
{
    stp_parameter_t description;
    std::memset(&description, 0, sizeof(description));
    stp_describe_parameter(vars, "Resolution", &description);
    if (description.bounds.str) {
        const size_t count = stp_string_list_count(description.bounds.str);
        for (size_t i = 0; i < count; ++i) {
            addResolutionProfile(vars, stp_string_list_param(description.bounds.str, i), profile);
        }
    }
    stp_parameter_description_destroy(&description);
}

void primeCatalogVars(stp_vars_t* vars)
{
    stp_set_string_parameter(vars, "PageSize", "Custom");
    stp_set_string_parameter(vars, "InputSlot", "Rear");
    stp_set_string_parameter(vars, "MediaType", "ColorOffset");
    stp_set_string_parameter(vars, "PrintingMode", "Color");
    stp_set_string_parameter(vars, "InputImageType", "Raw");
    stp_set_string_parameter(vars, "RawChannels", "6");
    stp_set_string_parameter(vars, "InkSet", "Physical");
    stp_set_string_parameter(vars, "InkType", "Physical6");
    stp_set_string_parameter(vars, "ColorCorrection", "Density");
    stp_set_string_parameter(vars, "DitherAlgorithm", "Adaptive");
}

void addCatalogPrinter(const stp_printer_t* printer,
                       gutenrip::worker::PrintDriverCatalog* response)
{
    stp_vars_t* vars = stp_vars_create();
    if (!vars) {
        response->add_warnings("Unable to create printer vars for " +
                               std::string(stp_printer_get_driver(printer)));
        return;
    }
    stp_set_driver(vars, stp_printer_get_driver(printer));
    primeCatalogVars(vars);

    const stpi_escp2_printer_t* escp2 = stpi_escp2_get_printer(vars);
    if (!escp2) {
        response->add_warnings("Unable to read ESC/P2 profile for " +
                               std::string(stp_printer_get_driver(printer)));
        stp_vars_destroy(vars);
        return;
    }

    auto* profile = response->add_drivers();
    const std::string label = stp_printer_get_long_name(printer)
        ? stp_printer_get_long_name(printer)
        : stp_printer_get_driver(printer);
    profile->set_printer_label(label);
    profile->set_printer_model(normalizedPrinterModel(label));
    profile->set_driver(stp_printer_get_driver(printer));
    profile->set_model_id(stp_printer_get_model(printer));
    profile->set_nozzles(escp2->nozzles);
    profile->set_nozzle_separation(escp2->nozzle_separation);
    profile->set_base_separation(escp2->base_separation);
    if (escp2->base_separation > 0 && escp2->nozzle_separation > 0 && escp2->nozzles > 0) {
        profile->set_base_padding_lines((escp2->base_separation / escp2->nozzle_separation) -
                                        escp2->nozzles);
    }
    const inklist_t* inkList = stpi_escp2_inklist(vars);
    if (inkList) {
        for (unsigned int inkIndex = 0; inkIndex < inkList->n_inks; ++inkIndex) {
            const inkname_t& inkName = inkList->inknames[inkIndex];
            if (!inkName.name || std::strcmp(inkName.name, "Physical6") != 0) {
                continue;
            }
            profile->set_channel_count(inkName.channel_count);
            int channelIndex = 0;
            for (unsigned int i = 0; i < inkName.channel_count; ++i) {
                const ink_channel_t& channel = inkName.channels[i];
                for (unsigned int j = 0; j < channel.n_subchannels; ++j) {
                    const physical_subchannel_t& subchannel = channel.subchannels[j];
                    auto* physical = profile->add_physical_channels();
                    physical->set_index(channelIndex++);
                    physical->set_esc_color(escIColorForSubchannel(subchannel));
                    physical->set_physical_head_offset(subchannel.head_offset);
                }
            }
            break;
        }
    }
    fillResolutions(vars, profile);

    stp_vars_destroy(vars);
}

void fillPrintDriverCatalog(gutenrip::worker::PrintDriverCatalog* response)
{
    const int printerCount = stp_printer_model_count();
    for (int i = 0; i < printerCount; ++i) {
        const stp_printer_t* printer = stp_get_printer_by_index(i);
        if (isCatalogPrinter(printer)) {
            addCatalogPrinter(printer, response);
        }
    }
}

double pageMMFromRequest(double mmValue, int pointValue, double fallbackMM)
{
    if (mmValue > 0.01) {
        return mmValue;
    }
    if (pointValue > 0) {
        return static_cast<double>(pointValue) * 25.4 / 72.0;
    }
    return fallbackMM;
}

std::string pageSizeForLayout(const gutenrip::worker::RenderPlaneRequest* request)
{
    (void)request;
    return "Custom";
}

int mmToPx(double mm, int dpi)
{
    return static_cast<int>(std::lround(mm * static_cast<double>(dpi) / 25.4));
}

stp_dimension_t mmToPoints(double mm)
{
    return static_cast<stp_dimension_t>(mm * 72.0 / 25.4);
}

LayoutImageContext layoutFromRequest(const gutenrip::worker::RenderPlaneRequest* request,
                                     bool isWhite)
{
    const int dpiX = positiveDpi(request->dpi_x());
    const int dpiY = positiveDpi(request->dpi_y());
    const double pageWidthMM = pageMMFromRequest(request->page_width_mm(),
                                                 request->page_width_points(),
                                                 210.0);
    const double pageHeightMM = pageMMFromRequest(request->page_height_mm(),
                                                  request->page_height_points(),
                                                  297.0);

    LayoutImageContext context;
    context.pageWidthMM = pageWidthMM;
    context.pageHeightMM = pageHeightMM;
    context.pageWidthPx = std::max(1, mmToPx(pageWidthMM, dpiX));
    context.pageHeightPx = std::max(1, mmToPx(pageHeightMM, dpiY));
    context.imageX = std::clamp(mmToPx(std::max(0.0, request->image_x_mm()), dpiX),
                                0,
                                context.pageWidthPx);
    context.imageY = std::clamp(mmToPx(std::max(0.0, request->image_y_mm()), dpiY),
                                0,
                                context.pageHeightPx);
    context.imageXMM = std::min(std::max(0.0, request->image_x_mm()), pageWidthMM);
    context.imageYMM = std::min(std::max(0.0, request->image_y_mm()), pageHeightMM);

    const double imageWidthMM = request->image_width_mm() > 0.01
        ? request->image_width_mm()
        : pageWidthMM - context.imageXMM;
    const double imageHeightMM = request->image_height_mm() > 0.01
        ? request->image_height_mm()
        : pageHeightMM - context.imageYMM;
    context.imageWidthMM = std::min(std::max(0.01, imageWidthMM),
                                    std::max(0.01, pageWidthMM - context.imageXMM));
    context.imageHeightMM = std::min(std::max(0.01, imageHeightMM),
                                     std::max(0.01, pageHeightMM - context.imageYMM));
    context.imageWidth = std::max(1, mmToPx(context.imageWidthMM, dpiX));
    context.imageHeight = std::max(1, mmToPx(context.imageHeightMM, dpiY));
    context.imageWidth = std::clamp(context.imageWidth, 1, std::max(1, context.pageWidthPx - context.imageX));
    context.imageHeight = std::clamp(context.imageHeight, 1, std::max(1, context.pageHeightPx - context.imageY));
    context.isWhite = isWhite;
    context.fill = isWhite ? static_cast<std::uint8_t>(0x55) : static_cast<std::uint8_t>(0xaa);
    return context;
}

bool loadPreparedRawImage(const std::string& inputPath, LayoutImageContext* context, std::string* error)
{
    if (!context || inputPath.empty()) {
        return false;
    }

    yink::rip::RawInteropImage image;
    std::string readError;
    if (!yink::rip::readRawInteropImage(inputPath, &image, &readError)) {
        if (error) {
            *error = readError.empty()
                ? "Unable to load raw interop image: " + inputPath
                : readError;
        }
        return false;
    }

    context->hasSourceImage = true;
    context->sourceWidth = image.width;
    context->sourceHeight = image.height;
    context->imageWidth = image.width;
    context->imageHeight = image.height;
    context->rawChannelCount = static_cast<std::size_t>(image.channels);
    context->rawPixels = std::move(image.pixels);
    context->sourceInputImageType = "Raw";
    return true;
}

std::string stringSetting(const gutenrip::worker::RenderPlaneRequest* request,
                          const std::string& key,
                          const std::string& fallback)
{
    const auto& settings = request->print_settings();
    const auto it = settings.find(key);
    if (it != settings.end() && !it->second.empty()) {
        return it->second;
    }
    return fallback;
}

double doubleSetting(const gutenrip::worker::RenderPlaneRequest* request,
                     const std::string& key,
                     double fallback)
{
    const std::string value = stringSetting(request, key, std::string());
    if (value.empty()) {
        return fallback;
    }
    char* end = nullptr;
    const double parsed = std::strtod(value.c_str(), &end);
    return end != value.c_str() ? parsed : fallback;
}

int intSetting(const gutenrip::worker::RenderPlaneRequest* request,
               const std::string& key,
               int fallback)
{
    const std::string value = stringSetting(request, key, std::string());
    if (value.empty()) {
        return fallback;
    }
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    return end != value.c_str() ? static_cast<int>(parsed) : fallback;
}

struct PrintedDpi {
    int x = 0;
    int y = 0;
};

PrintedDpi printedDpiFromResolution(const res_t* resolution)
{
    if (!resolution) {
        return {};
    }
    return {
        resolution->printed_hres > 0 ? resolution->printed_hres : resolution->hres,
        resolution->printed_vres > 0 ? resolution->printed_vres : resolution->vres,
    };
}

PrintedDpi printedDpiFromResolutionName(const std::string& resolution)
{
    const std::size_t x = resolution.find_first_of("xX");
    if (x == std::string::npos || x + 1 >= resolution.size()) {
        return {};
    }
    char* xEnd = nullptr;
    const long parsedX = std::strtol(resolution.c_str(), &xEnd, 10);
    char* yEnd = nullptr;
    const long parsedY = std::strtol(resolution.c_str() + x + 1, &yEnd, 10);
    return {
        xEnd != resolution.c_str() ? static_cast<int>(parsedX) : 0,
        yEnd != resolution.c_str() + x + 1 ? static_cast<int>(parsedY) : 0,
    };
}

double escIFeatherEdgeForResolution(double edge, const PrintedDpi& printedDpi)
{
    if (printedDpi.x <= 0 || printedDpi.y <= 0) {
        return 1.0;
    }
    return edge;
}

PrintedDpi activePrintedDpiForResolution(stp_vars_t* vars,
                                         const std::string& resolution)
{
    if (vars && !resolution.empty()) {
        stp_set_string_parameter(vars, "Resolution", resolution.c_str());
    }
    PrintedDpi printedDpi = printedDpiFromResolution(stpi_escp2_find_resolution(vars));
    if (printedDpi.x <= 0 || printedDpi.y <= 0) {
        printedDpi = printedDpiFromResolutionName(resolution);
    }
    return printedDpi;
}

bool boolSetting(const gutenrip::worker::RenderPlaneRequest* request,
                 const std::string& key,
                 bool fallback)
{
    std::string value = stringSetting(request, key, std::string());
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (value == "1" || value == "true" || value == "yes" || value == "on") {
        return true;
    }
    if (value == "0" || value == "false" || value == "no" || value == "off") {
        return false;
    }
    return fallback;
}

std::string defaultWhiteInkTypeFor(const std::string& colorInkType)
{
    return colorInkType;
}

bool whiteInkTypeUsesRgbInput(const std::string& whiteInkType)
{
    (void)whiteInkType;
    return false;
}

std::size_t rawChannelCountForInkType(const std::string& inkType)
{
    (void)inkType;
    return 6u;
}

double neutralRawInkLimitForChannelCount(std::size_t channelCount)
{
    return std::max(1.0, static_cast<double>(channelCount));
}

double boundedDoubleSetting(const gutenrip::worker::RenderPlaneRequest* request,
                            const std::string& key,
                            double fallback,
                            double minimum,
                            double maximum)
{
    const double value = doubleSetting(request, key, fallback);
    if (!std::isfinite(value)) {
        return std::clamp(fallback, minimum, maximum);
    }
    return std::clamp(value, minimum, maximum);
}

std::vector<double> rawChannelDensities(
    const gutenrip::worker::RenderPlaneRequest* request,
    std::size_t channelCount)
{
    std::vector<double> result;
    result.reserve(channelCount);
    for (std::size_t i = 0; i < channelCount; ++i) {
        result.push_back(boundedDoubleSetting(
            request,
            "RawChannelDensity" + std::to_string(i),
            1.0,
            0.0,
            1.0));
    }
    return result;
}

std::string rawChannelDensitySummary(const std::vector<double>& densities)
{
    std::ostringstream summary;
    for (std::size_t i = 0; i < densities.size(); ++i) {
        if (i != 0) {
            summary << ',';
        }
        summary << densities[i];
    }
    return summary.str();
}

bool rawChannelHasNonZero(const LayoutImageContext& context, std::size_t channel)
{
    if (context.rawChannelCount == 0 || channel >= context.rawChannelCount || context.rawPixels.empty()) {
        return false;
    }

    const std::size_t pixels = context.rawPixels.size() / context.rawChannelCount;
    for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
        if (context.rawPixels[pixel * context.rawChannelCount + channel] != 0) {
            return true;
        }
    }
    return false;
}

bool rawChannelsHaveNonZero(const LayoutImageContext& context,
                            std::size_t firstChannel,
                            std::size_t lastChannelExclusive)
{
    const std::size_t end = std::min(lastChannelExclusive, context.rawChannelCount);
    for (std::size_t channel = firstChannel; channel < end; ++channel) {
        if (rawChannelHasNonZero(context, channel)) {
            return true;
        }
    }
    return false;
}

std::string normalizedRawInkTypeFor(const std::string& inkType)
{
    (void)inkType;
    return "Physical6";
}

yink::rip::ComposeOptions composeOptionsFromProto(const gutenrip::worker::ComposeSettings& source)
{
    yink::rip::ComposeOptions options;
    options.layerOrder = source.layer_order() == "WhiteOverColor"
        ? yink::rip::LayerOrder::WhiteOverColor
        : yink::rip::LayerOrder::WhiteUnderColor;
    options.frontHalfPrintsFirst = source.front_half_prints_first();
    if (source.source_light_cyan_white_color() > 0) {
        options.sourceLightCyanWhiteColor =
            static_cast<std::uint8_t>(source.source_light_cyan_white_color());
    }
    if (source.source_light_magenta_white_color() > 0) {
        options.sourceLightMagentaWhiteColor =
            static_cast<std::uint8_t>(source.source_light_magenta_white_color());
    }
    if (source.light_cyan_white_color() > 0) {
        options.lightCyanWhiteColor = static_cast<std::uint8_t>(source.light_cyan_white_color());
    }
    if (source.light_magenta_white_color() > 0) {
        options.lightMagentaWhiteColor = static_cast<std::uint8_t>(source.light_magenta_white_color());
    }
    if (source.nozzles() > 0) {
        options.nozzles = source.nozzles();
    }
    if (source.nozzle_separation() > 0) {
        options.nozzleSeparation = source.nozzle_separation();
    }
    if (source.base_separation() > 0) {
        options.baseSeparation = source.base_separation();
    }
    options.baseChannelColors.reserve(static_cast<std::size_t>(source.base_channel_colors_size()));
    for (const int color : source.base_channel_colors()) {
        options.baseChannelColors.push_back(color);
    }
    return options;
}

std::string rawInkSetForInkType(const std::string& inkType)
{
    (void)inkType;
    return "Physical";
}

bool isLayeredRawDriver(const std::string& driver)
{
    return driver == "escp2-l18050" || driver == "escp2-et8550" ||
           driver.rfind("escp2-l18050-", 0) == 0 ||
           driver.rfind("escp2-et8550-", 0) == 0;
}

std::string rawMediaTypeForDriver(const std::string& driver,
                                  const std::string& requestedMediaType,
                                  const std::string& inkSet)
{
    if (isLayeredRawDriver(driver)) {
        (void)inkSet;
        return "ColorOffset";
    }
    return requestedMediaType.empty() ? std::string("Plain") : requestedMediaType;
}

std::string rawDitherAlgorithmForDriver(const std::string& driver,
                                        bool isWhite,
                                        const gutenrip::worker::RenderPlaneRequest* request)
{
    if (isWhite && isLayeredRawDriver(driver)) {
        return "VeryFast";
    }
    return stringSetting(request, "DitherAlgorithm", "Adaptive");
}

yink::rip::WhitePlateMode whitePlateModeFromString(const std::string& value)
{
    if (value == "FillAll") {
        return yink::rip::WhitePlateMode::FillAll;
    }
    if (value == "GradientNegative") {
        return yink::rip::WhitePlateMode::GradientNegative;
    }
    if (value == "GradientPositive") {
        return yink::rip::WhitePlateMode::GradientPositive;
    }
    if (value == "Alpha") {
        return yink::rip::WhitePlateMode::Alpha;
    }
    return yink::rip::WhitePlateMode::AnyColoredPixel;
}

void setStringParameter(stp_vars_t* vars, const char* key, const std::string& value)
{
    if (!value.empty()) {
        stp_set_string_parameter(vars, key, value.c_str());
    }
}

void setFloatParameter(stp_vars_t* vars, const char* key, double value)
{
    stp_set_float_parameter(vars, key, value);
}

void setIntParameter(stp_vars_t* vars, const char* key, int value)
{
    stp_set_int_parameter(vars, key, value);
}

void setBoolParameter(stp_vars_t* vars, const char* key, bool value)
{
    stp_set_boolean_parameter(vars, key, value ? 1 : 0);
}

struct ResolvedChannel {
    std::string source;
    int escColor = 0;
    int physicalHeadOffset = 0;
    int runtimeHeadOffset = 0;
    int correctionRows = 0;
    double density = 1.0;
};

struct ResolvedChannelLayout {
    bool ok = false;
    std::string error;
    bool runtimeGeometryEnabled = true;
    bool offsetCorrectionEnabled = true;
    int physicalNozzles = 0;
    int activeNozzles = 0;
    int nozzleSeparation = 0;
    std::vector<ResolvedChannel> channels;
};

bool isLogicalInk(const std::string& source)
{
    return source == "C" || source == "M" || source == "Y" ||
           source == "K" || source == "W" || source == "Off";
}

std::vector<std::string> defaultChannelMapping(const std::string& driver)
{
    if (driver.find("et8550") != std::string::npos) {
        return {"K", "W", "W", "Y", "M", "C"};
    }
    return {"K", "W", "W", "C", "Y", "M"};
}

const inkname_t* physicalInkName(stp_vars_t* vars)
{
    const inklist_t* inkList = stpi_escp2_inklist(vars);
    if (!inkList) {
        return nullptr;
    }
    for (unsigned int i = 0; i < inkList->n_inks; ++i) {
        if (inkList->inknames[i].name &&
            std::strcmp(inkList->inknames[i].name, "Physical6") == 0) {
            return &inkList->inknames[i];
        }
    }
    return nullptr;
}

ResolvedChannelLayout resolveChannelLayout(
    stp_vars_t* vars,
    const std::string& driver,
    const std::string& resolution,
    const gutenrip::worker::ComposeSettings& settings,
    const std::vector<double>& channelDensities)
{
    ResolvedChannelLayout result;
    const stpi_escp2_printer_t* printer = stpi_escp2_get_printer(vars);
    const inkname_t* inkName = physicalInkName(vars);
    if (!printer || !inkName) {
        result.error = "Unable to resolve physical raw channel layout.";
        return result;
    }

    std::vector<std::string> mapping;
    mapping.reserve(static_cast<std::size_t>(settings.channel_mapping_size()));
    for (const auto& source : settings.channel_mapping()) {
        mapping.push_back(source);
    }
    if (mapping.empty()) {
        mapping = defaultChannelMapping(driver);
    }

    int physicalChannelCount = 0;
    for (unsigned int i = 0; i < inkName->channel_count; ++i) {
        physicalChannelCount += inkName->channels[i].n_subchannels;
    }
    if (mapping.size() != static_cast<std::size_t>(physicalChannelCount)) {
        result.error = "Channel mapping count does not match the physical raw channel count.";
        return result;
    }
    if (channelDensities.size() != static_cast<std::size_t>(physicalChannelCount)) {
        result.error = "Raw channel density count does not match the physical raw channel count.";
        return result;
    }
    for (const auto& source : mapping) {
        if (!isLogicalInk(source)) {
            result.error = "Channel mapping contains an unsupported logical ink.";
            return result;
        }
    }

    result.physicalNozzles = printer->nozzles;
    result.nozzleSeparation = printer->nozzle_separation;
    const bool combinedPrint = settings.print_content_mode().empty() ||
        settings.print_content_mode() == "WhiteAndColor";
    result.runtimeGeometryEnabled = combinedPrint;
    result.offsetCorrectionEnabled = combinedPrint;
    result.activeNozzles = combinedPrint
        ? (resolution.find("720x1440") != std::string::npos ? 83 : 86)
        : result.physicalNozzles;
    if (result.physicalNozzles <= 0 || result.nozzleSeparation <= 0 ||
        result.activeNozzles <= 0 || result.activeNozzles > result.physicalNozzles) {
        result.error = "Resolved nozzle geometry is invalid.";
        return result;
    }

    const int automaticHeadOffset =
        (result.physicalNozzles - result.activeNozzles) * result.nozzleSeparation;
    int physicalIndex = 0;
    for (unsigned int i = 0; i < inkName->channel_count; ++i) {
        const ink_channel_t& channel = inkName->channels[i];
        for (unsigned int j = 0; j < channel.n_subchannels; ++j) {
            const physical_subchannel_t& subchannel = channel.subchannels[j];
            const std::string& source = mapping[static_cast<std::size_t>(physicalIndex)];

            ResolvedChannel resolved;
            resolved.source = source;
            resolved.escColor = escIColorForSubchannel(subchannel);
            resolved.physicalHeadOffset = subchannel.head_offset;
            if (result.runtimeGeometryEnabled) {
                const bool white = source == "W";
                const bool whiteOverColor = settings.layer_order() == "WhiteOverColor";
                const bool offsetChannel = whiteOverColor ? white : !white;
                resolved.runtimeHeadOffset = offsetChannel ? automaticHeadOffset : 0;
                resolved.correctionRows = resolved.runtimeHeadOffset == 0
                    ? automaticHeadOffset / result.nozzleSeparation
                    : 0;
            }
            resolved.density = channelDensities[static_cast<std::size_t>(physicalIndex)];
            result.channels.push_back(std::move(resolved));
            ++physicalIndex;
        }
    }
    result.ok = true;
    return result;
}

void applyChannelLayout(stp_vars_t* vars, const ResolvedChannelLayout& layout)
{
    if (layout.runtimeGeometryEnabled) {
        setIntParameter(vars, "RawActiveNozzles", layout.activeNozzles);
    }
    for (std::size_t i = 0; i < layout.channels.size(); ++i) {
        const std::string suffix = std::to_string(i);
        if (layout.runtimeGeometryEnabled) {
            setIntParameter(vars,
                            ("RawHeadOffset" + suffix).c_str(),
                            layout.channels[i].runtimeHeadOffset);
            setIntParameter(vars,
                            ("RawChannelCorrectionRows" + suffix).c_str(),
                            layout.channels[i].correctionRows);
        }
        setFloatParameter(vars,
                          ("RawChannelDensity" + suffix).c_str(),
                          layout.channels[i].density);
    }
}

yink::rip::ChannelOffsetCorrectionOptions correctionOptionsForLayout(
    const gutenrip::worker::ComposeSettings& source,
    const ResolvedChannelLayout& layout)
{
    auto options = composeOptionsFromProto(source);
    options.nozzles = layout.activeNozzles;
    options.nozzleSeparation = layout.nozzleSeparation;
    options.baseSeparation = layout.physicalNozzles * layout.nozzleSeparation;
    options.baseChannelColors.clear();
    options.channelCorrections.clear();
    options.channelCorrections.reserve(layout.channels.size());
    for (const auto& channel : layout.channels) {
        options.channelCorrections.push_back(
            {channel.escColor, channel.correctionRows});
    }
    return options;
}

bool writeOutputBytes(OutputContext* ctx, const std::uint8_t* bytes, std::size_t size)
{
    if (!ctx || !bytes || size == 0) {
        return true;
    }
    if (ctx->stream) {
        gutenrip::worker::RenderPlaneStreamResponse response;
        auto* chunk = response.mutable_prn_chunk();
        chunk->set_data(reinterpret_cast<const char*>(bytes), size);
        chunk->set_bytes_written(static_cast<std::uint64_t>(ctx->bytesWritten + size));
        if (!ctx->stream->Write(response)) {
            ctx->writeFailed = true;
            return false;
        }
        ctx->bytesWritten += size;
        return true;
    }
    if (ctx->out.is_open()) {
        ctx->out.write(reinterpret_cast<const char*>(bytes), static_cast<std::streamsize>(size));
        ctx->bytesWritten += size;
        return true;
    }
    ctx->writeFailed = true;
    return false;
}

bool writeOutputBytes(OutputContext* ctx, const std::vector<std::uint8_t>& bytes)
{
    return writeOutputBytes(ctx, bytes.data(), bytes.size());
}

void outputFunc(void* data, const char* buffer, std::size_t bytes)
{
    auto* ctx = static_cast<OutputContext*>(data);
    if (!ctx || ctx->writeFailed) {
        return;
    }
    if (ctx->offsetCorrector) {
        const auto rewritten = ctx->offsetCorrector->append(
            reinterpret_cast<const std::uint8_t*>(buffer), bytes);
        (void)writeOutputBytes(ctx, rewritten);
    } else {
        (void)writeOutputBytes(ctx, reinterpret_cast<const std::uint8_t*>(buffer), bytes);
    }
}

bool fillStreamLayoutRow(LayoutImageContext* context, unsigned char* data,
                         std::size_t byteLimit, int row)
{
    std::memset(data, 0, byteLimit);
    if (!context || !context->stream) {
        return false;
    }
    if (row < 0 || row >= context->imageHeight) {
        context->streamError = "Streaming RAW row request was out of range.";
        return false;
    }

    gutenrip::worker::RenderPlaneStreamResponse response;
    auto* request = response.mutable_raw_row_request();
    request->set_row(row);
    request->set_byte_limit(static_cast<std::uint64_t>(byteLimit));
    if (!context->stream->Write(response)) {
        context->streamError = "Unable to request streaming RAW row.";
        return false;
    }

    gutenrip::worker::RenderPlaneStreamRequest input;
    if (!context->stream->Read(&input) || !input.has_raw_row()) {
        context->streamError = "Streaming RAW row response was missing.";
        return false;
    }
    const auto& rawRow = input.raw_row();
    if (rawRow.row() != row) {
        context->streamError = "Streaming RAW row response did not match requested row.";
        return false;
    }
    const std::string& bytes = rawRow.data();
    const std::size_t copyBytes = std::min(byteLimit, bytes.size());
    if (copyBytes > 0) {
        std::memcpy(data, bytes.data(), copyBytes);
    }
    return true;
}

void fillLayoutRow(LayoutImageContext& context, unsigned char* data,
                   std::size_t byteLimit, int row)
{
    std::memset(data, 0, byteLimit);
    if (context.stream) {
        (void)fillStreamLayoutRow(&context, data, byteLimit, row);
        return;
    }
    if (context.imageWidth <= 0 || context.imageHeight <= 0) {
        return;
    }
    if (row < 0 || row >= context.imageHeight) {
        return;
    }

    const std::size_t channels = context.rawChannelCount > 0
        ? context.rawChannelCount
        : 1u;
    const std::size_t rowPixels = static_cast<std::size_t>(context.imageWidth) * channels;
    const std::size_t rowBytes = rowPixels * sizeof(std::uint16_t);
    if (context.hasSourceImage && !context.rawPixels.empty()) {
        const std::size_t offset = static_cast<std::size_t>(row) * rowPixels;
        if (offset >= context.rawPixels.size()) {
            return;
        }
        const std::size_t available = std::min(rowBytes, (context.rawPixels.size() - offset) * sizeof(std::uint16_t));
        std::memcpy(data, context.rawPixels.data() + offset, std::min(byteLimit, available));
        return;
    }

    const std::size_t activeBytes = std::min(byteLimit, rowBytes);
    if (activeBytes > 0) {
        std::memset(data, context.fill, activeBytes);
    }
}

stp_image_status_t getRowFunc(stp_image_t* image, unsigned char* data,
                              std::size_t byteLimit, int row)
{
    if (!image || !image->rep) {
        std::memset(data, 0, byteLimit);
        return STP_IMAGE_STATUS_OK;
    }
    auto* context = static_cast<LayoutImageContext*>(image->rep);
    fillLayoutRow(*context, data, byteLimit, row);
    if (!context->streamError.empty()) {
        return STP_IMAGE_STATUS_ABORT;
    }
    return STP_IMAGE_STATUS_OK;
}

int imageWidthFunc(stp_image_t* image)
{
    const auto* context = image ? static_cast<const LayoutImageContext*>(image->rep) : nullptr;
    return context ? context->imageWidth : 1;
}

int imageHeightFunc(stp_image_t* image)
{
    const auto* context = image ? static_cast<const LayoutImageContext*>(image->rep) : nullptr;
    return context ? context->imageHeight : 1;
}

const char* appNameFunc(stp_image_t*)
{
    return "guten-rip-worker";
}

std::vector<std::uint8_t> makeLayoutSyntheticEscIPrn(std::uint8_t color,
                                                     const LayoutImageContext& context)
{
    const int bytesPerLine = std::clamp((context.imageWidth + 7) / 8, 1, 65535);
    const int lineCount = std::clamp(context.imageHeight, 1, 65535);
    std::vector<std::uint8_t> bytes = {
        0x1b, '@',
        0x1b, 'i',
        color,
        0x00,
        0x01,
        static_cast<std::uint8_t>(bytesPerLine & 0xff),
        static_cast<std::uint8_t>((bytesPerLine >> 8) & 0xff),
        static_cast<std::uint8_t>(lineCount & 0xff),
        static_cast<std::uint8_t>((lineCount >> 8) & 0xff),
    };

    const std::size_t headerSize = bytes.size();
    bytes.resize(headerSize + static_cast<std::size_t>(bytesPerLine) * lineCount);
    LayoutImageContext mutableContext = context;
    for (int row = 0; row < lineCount; ++row) {
        fillLayoutRow(mutableContext,
                      bytes.data() + headerSize + static_cast<std::size_t>(row) * bytesPerLine,
                      static_cast<std::size_t>(bytesPerLine),
                      row);
    }
    bytes.push_back('\r');
    bytes.push_back('\f');
    return bytes;
}

} // namespace

RipWorkerService::RipWorkerService()
{
    configureGutenprintDebugFlags();
    configureGutenprintDataPath();
    stp_init();

    const int modelCount = stp_printer_model_count();
    if (modelCount == 0) {
        std::cerr << "[guten-rip-worker] WARNING: stp_init completed but no printer models found."
                  << " STP_DATA_PATH=" << (std::getenv("STP_DATA_PATH") ? std::getenv("STP_DATA_PATH") : "(not set)")
                  << std::endl;
    } else {
        int catalogCount = 0;
        for (int i = 0; i < modelCount; ++i) {
            if (isCatalogPrinter(stp_get_printer_by_index(i))) {
                ++catalogCount;
            }
        }
        if (catalogCount == 0) {
            std::cerr << "[guten-rip-worker] WARNING: stp_init found " << modelCount
                      << " printer models but no 9xx catalog drivers."
                      << " STP_DATA_PATH=" << (std::getenv("STP_DATA_PATH") ? std::getenv("STP_DATA_PATH") : "(not set)")
                      << std::endl;
        }
    }
}

RipWorkerService::~RipWorkerService()
{
}

grpc::Status RipWorkerService::GetCapabilities(grpc::ServerContext* context,
                                               const gutenrip::worker::Empty* request,
                                               gutenrip::worker::Capabilities* response)
{
    (void)context;
    (void)request;

    response->set_worker_name("guten-rip-worker");
    gutenrip::worker::PrintDriverCatalog catalog;
    fillPrintDriverCatalog(&catalog);
    if (catalog.drivers_size() == 0) {
        std::cerr << "[guten-rip-worker] GetCapabilities: no catalog drivers, reporting empty default."
                  << " STP_DATA_PATH=" << (std::getenv("STP_DATA_PATH") ? std::getenv("STP_DATA_PATH") : "(not set)")
                  << std::endl;
    }
    if (catalog.drivers_size() > 0) {
        response->set_default_printer_driver(catalog.drivers(0).driver());
    }

    auto* printer = response->add_capabilities();
    printer->set_key("printers");
    std::set<std::string> printerLabels;
    for (const auto& driver : catalog.drivers()) {
        if (printerLabels.insert(driver.printer_label()).second) {
            printer->add_values(driver.printer_label());
        }
    }

    auto* resolutions = response->add_capabilities();
    resolutions->set_key("resolutions");
    std::set<std::string> resolutionNames;
    for (const auto& driver : catalog.drivers()) {
        for (const auto& resolution : driver.resolutions()) {
            if (resolutionNames.insert(resolution.name()).second) {
                resolutions->add_values(resolution.name());
            }
        }
    }

    auto* planes = response->add_capabilities();
    planes->set_key("planes");
    planes->add_values("Color");
    planes->add_values("White");
    return grpc::Status::OK;
}

grpc::Status RipWorkerService::GetPrintDriverCatalog(
    grpc::ServerContext* context,
    const gutenrip::worker::Empty* request,
    gutenrip::worker::PrintDriverCatalog* response)
{
    (void)context;
    (void)request;

    fillPrintDriverCatalog(response);
    return grpc::Status::OK;
}

grpc::Status RipWorkerService::RenderPlane(grpc::ServerContext* context,
                                           const gutenrip::worker::RenderPlaneRequest* request,
                                           gutenrip::worker::RenderPlaneResult* response)
{
    (void)context;

    const bool isWhite = request->plane() == gutenrip::worker::PLANE_WHITE;
    const std::string colorInkType = normalizedRawInkTypeFor(stringSetting(request, "InkType", "Physical6"));
    const std::string whiteInkType = normalizedRawInkTypeFor(stringSetting(request, "WhiteInkType",
                                                   defaultWhiteInkTypeFor(colorInkType)));
    const std::string inkType = isWhite ? whiteInkType : colorInkType;
    const std::string requestedWhitePrintingMode = stringSetting(request, "WhitePrintingMode", "BW");
    const std::string printingMode = isWhite
        ? std::string("Color")
        : stringSetting(request, "ColorPrintingMode", "Color");
    const std::string requestedInputImageType = stringSetting(request, "InputImageType", "Auto");
    const std::string requestedPrinterDriver = request->printer_driver().empty()
        ? defaultCatalogDriver()
        : request->printer_driver();
    const std::string printerDriver = requestedPrinterDriver;
    const stp_printer_t* printer = stp_get_printer_by_driver(printerDriver.c_str());
    if (!printer) {
        response->set_ok(false);
        response->set_message("Printer driver not found: " + printerDriver);
        return grpc::Status::OK;
    }

    stp_vars_t* vars = stp_vars_create_copy(stp_printer_get_defaults(printer));
    if (!vars) {
        response->set_ok(false);
        response->set_message("Unable to create printer vars.");
        return grpc::Status::OK;
    }

    LayoutImageContext layoutContext = layoutFromRequest(request, isWhite);
    std::string sourceLoadError;
    const bool loadedSource = loadPreparedRawImage(request->input_path(), &layoutContext, &sourceLoadError);
    if (!loadedSource && !request->input_path().empty()) {
        stp_vars_destroy(vars);
        response->set_ok(false);
        response->set_message(sourceLoadError.empty()
                                  ? "Unable to load raw interop image: " + request->input_path()
                                  : sourceLoadError);
        response->set_bytes_written(0);
        return grpc::Status::OK;
    }

    if (!loadedSource) {
        layoutContext.rawChannelCount = rawChannelCountForInkType(inkType);
    } else if (layoutContext.rawChannelCount != rawChannelCountForInkType(inkType)) {
        stp_vars_destroy(vars);
        response->set_ok(false);
        response->set_message("Raw channel count does not match selected InkType=" + inkType +
                              ": got " + std::to_string(layoutContext.rawChannelCount) +
                              ", expected " + std::to_string(rawChannelCountForInkType(inkType)));
        response->set_bytes_written(0);
        return grpc::Status::OK;
    }
    const std::string inputImageType = "Raw";
    const std::string requestedMediaType = stringSetting(request, "MediaType", "Plain");
    const std::string inkSet = isLayeredRawDriver(printerDriver)
        ? rawInkSetForInkType(inkType)
        : stringSetting(request, "InkSet", "None");
    const std::string mediaType = rawMediaTypeForDriver(printerDriver, requestedMediaType, inkSet);
    const std::string ditherAlgorithm = rawDitherAlgorithmForDriver(printerDriver, isWhite, request);
    const std::string resolution = stringSetting(request, "Resolution", "1440x1440ov");
    const double rawDensity = boundedDoubleSetting(request,
                                                   "Density",
                                                   request->density() > 0.0 ? request->density() : 1.0,
                                                   0.0,
                                                   1.0);
    const double outputInkLimit = boundedDoubleSetting(
        request,
        "OutputInkLimit",
        doubleSetting(request, "InkLimit", neutralRawInkLimitForChannelCount(layoutContext.rawChannelCount)),
        0.01,
        64.0);
    const bool rawHasColorInk = loadedSource && rawChannelsHaveNonZero(layoutContext, 0u, 4u);
    const bool rawHasWhiteInk = loadedSource && rawChannelsHaveNonZero(layoutContext, 4u, 6u);
    const double rawInkLimit = outputInkLimit;
    const auto channelDensities = rawChannelDensities(request, layoutContext.rawChannelCount);
    const PrintedDpi printedDpi = activePrintedDpiForResolution(vars, resolution);
    const double rawEscIFeatherEdge =
        escIFeatherEdgeForResolution(
            boundedDoubleSetting(request, "EscIFeatherEdge", 1.0, 0.0, 1.0),
            printedDpi);
    int bandEnhancement = intSetting(request, "BandEnhancement", 0);
    const std::string colorCorrection = stringSetting(request, "ColorCorrection", "Density");
    setStringParameter(vars, "PageSize", pageSizeForLayout(request));
    setStringParameter(vars, "InputSlot", stringSetting(request, "InputSlot", "Rear"));
    setStringParameter(vars, "Duplex", stringSetting(request, "Duplex", "None"));
    setStringParameter(vars, "CDInnerRadius", stringSetting(request, "CDInnerRadius", "None"));
    setStringParameter(vars, "MediaType", mediaType);
    setStringParameter(vars, "Resolution", resolution);
    setStringParameter(vars, "Quality", stringSetting(request, "Quality", "Standard"));
    setStringParameter(vars, "PrintingMode", printingMode);
    setStringParameter(vars, "InputImageType", inputImageType);
    setStringParameter(vars, "RawChannels", std::to_string(layoutContext.rawChannelCount));
    setStringParameter(vars, "InkSet", inkSet);
    setStringParameter(vars, "InkType", inkType);
    setStringParameter(vars, "ColorCorrection", colorCorrection);
    setStringParameter(vars, "DitherAlgorithm", ditherAlgorithm);
    setStringParameter(vars, "ChannelBitDepth", stringSetting(request, "ChannelBitDepth", "16"));
    setStringParameter(vars, "PrintingDirection", stringSetting(request, "PrintingDirection", "Bidirectional"));
    setStringParameter(vars, "Weave", stringSetting(request, "Weave", "None"));
    setIntParameter(vars, "BandEnhancement", bandEnhancement);
    setFloatParameter(vars, "Density", rawDensity);
    setFloatParameter(vars, "InkLimit", rawInkLimit);
    for (std::size_t i = 0; i < channelDensities.size(); ++i) {
        setFloatParameter(vars,
                          ("RawChannelDensity" + std::to_string(i)).c_str(),
                          channelDensities[i]);
    }
    setFloatParameter(vars, "EscIFeatherEdge", rawEscIFeatherEdge);
    setFloatParameter(vars, "Brightness", doubleSetting(request, "Brightness", 1.0));
    setFloatParameter(vars, "Contrast", doubleSetting(request, "Contrast", 1.0));
    setFloatParameter(vars, "Saturation", doubleSetting(request, "Saturation", 1.0));
    setFloatParameter(vars, "Gamma", doubleSetting(request, "Gamma", 1.0));

    const stp_dimension_t pageWidth = mmToPoints(pageMMFromRequest(request->page_width_mm(),
                                                                   request->page_width_points(),
                                                                   210.0));
    const stp_dimension_t pageHeight = mmToPoints(pageMMFromRequest(request->page_height_mm(),
                                                                    request->page_height_points(),
                                                                    297.0));
    constexpr stp_dimension_t minMargin = 0.0;
    stp_set_page_width(vars, pageWidth);
    stp_set_page_height(vars, pageHeight);
    stp_set_left(vars, std::max<stp_dimension_t>(minMargin, mmToPoints(layoutContext.imageXMM)));
    stp_set_top(vars, std::max<stp_dimension_t>(minMargin, mmToPoints(layoutContext.imageYMM)));
    stp_set_width(vars, std::max<stp_dimension_t>(1.0, mmToPoints(layoutContext.imageWidthMM)));
    stp_set_height(vars, std::max<stp_dimension_t>(1.0, mmToPoints(layoutContext.imageHeightMM)));

    OutputContext outputCtx;
    outputCtx.out.open(request->output_path(), std::ios::binary);
    if (!outputCtx.out) {
        stp_vars_destroy(vars);
        response->set_ok(false);
        response->set_message("Unable to create output PRN.");
        return grpc::Status::OK;
    }

    stp_set_outfunc(vars, outputFunc);
    stp_set_outdata(vars, &outputCtx);

    stp_image_t image;
    std::memset(&image, 0, sizeof(image));
    image.init = nullptr;
    image.reset = nullptr;
    image.width = imageWidthFunc;
    image.height = imageHeightFunc;
    image.get_row = getRowFunc;
    image.get_appname = appNameFunc;
    image.conclude = nullptr;
    image.rep = &layoutContext;

    std::cerr << "[guten-rip-worker] RenderPlane begin"
              << " job_id=" << request->job_id()
              << " plane=" << (isWhite ? "white" : "color")
              << " requestedPrinter=" << requestedPrinterDriver
              << " printer=" << printerDriver
              << " colorInkType=" << colorInkType
              << " whiteInkType=" << whiteInkType
              << " selectedInkType=" << inkType
              << " printingMode=" << printingMode
              << " requestedWhitePrintingMode=" << requestedWhitePrintingMode
              << " requestedMediaType=" << requestedMediaType
              << " mediaType=" << mediaType
              << " inkSet=" << inkSet
              << " inputImageType=" << inputImageType
              << " requestedInputImageType=" << requestedInputImageType
              << " detectedInputImageType=" << layoutContext.sourceInputImageType
              << " ditherAlgorithm=" << ditherAlgorithm
              << " colorCorrection=" << colorCorrection
              << " density=" << rawDensity
              << " inkLimit=" << rawInkLimit
              << " outputInkLimit=" << outputInkLimit
              << " rawChannelDensities=" << rawChannelDensitySummary(channelDensities)
              << " escIFeatherEdge=" << rawEscIFeatherEdge
              << " bandEnhancement=" << bandEnhancement
              << " rawHasColorInk=" << (rawHasColorInk ? "true" : "false")
              << " rawHasWhiteInk=" << (rawHasWhiteInk ? "true" : "false")
              << " rawChannels=" << layoutContext.rawChannelCount
              << " loadedSource=" << (loadedSource ? "true" : "false")
              << " rawPixels=" << layoutContext.rawPixels.size()
              << " pageMM=" << layoutContext.pageWidthMM << "x" << layoutContext.pageHeightMM
              << " imageMM=" << layoutContext.imageXMM << "," << layoutContext.imageYMM
              << "," << layoutContext.imageWidthMM << "x" << layoutContext.imageHeightMM
              << " imagePx=" << layoutContext.imageWidth << "x" << layoutContext.imageHeight
              << " dpi=" << positiveDpi(request->dpi_x()) << "x" << positiveDpi(request->dpi_y())
              << " output=" << request->output_path()
              << " STP_DATA_PATH=" << (std::getenv("STP_DATA_PATH") ? std::getenv("STP_DATA_PATH") : "")
              << " STP_DEBUG=" << (std::getenv("STP_DEBUG") ? std::getenv("STP_DEBUG") : "")
              << std::endl;

    const int printResult = stp_print(vars, &image);

    std::cerr << "[guten-rip-worker] RenderPlane stp_print returned"
              << " job_id=" << request->job_id()
              << " selectedInkType=" << inkType
              << " result=" << printResult
              << " bytes=" << outputCtx.bytesWritten
              << std::endl;

    outputCtx.out.close();

    if (printResult != 1 || outputCtx.bytesWritten == 0) {
        stp_vars_destroy(vars);
        std::remove(request->output_path().c_str());
        response->set_ok(false);
        response->set_message("Gutenprint render failed; synthetic fallback is disabled for PRN output. stp_print=" +
                              std::to_string(printResult) + ", bytes=" +
                              std::to_string(outputCtx.bytesWritten));
        response->set_bytes_written(0);
        return grpc::Status::OK;
    }

    stp_vars_destroy(vars);

    response->set_ok(true);
    response->set_message(std::string(loadedSource
                               ? "Rendered raw interop image via libgutenprint."
                               : "Rendered synthetic raw fallback via libgutenprint.") +
                           " InputImageType=" + inputImageType +
                            ", PrinterDriver=" + printerDriver +
                            ", MediaType=" + mediaType +
                            ", InkSet=" + inkSet +
                            ", RawChannels=" + std::to_string(layoutContext.rawChannelCount) +
                            ", PrintingMode=" + printingMode +
                            ", DitherAlgorithm=" + ditherAlgorithm +
                            ", ColorCorrection=" + colorCorrection +
                            ", Density=" + std::to_string(rawDensity) +
                            ", InkLimit=" + std::to_string(rawInkLimit) +
                            ", OutputInkLimit=" + std::to_string(outputInkLimit) +
                            ", RawChannelDensities=" + rawChannelDensitySummary(channelDensities) +
                            ", source=" + layoutContext.sourceInputImageType + ".");
    response->set_output_path(request->output_path());
    response->set_bytes_written(outputCtx.bytesWritten);
    return grpc::Status::OK;
}

grpc::Status RipWorkerService::RenderPlaneStream(
    grpc::ServerContext* context,
    grpc::ServerReaderWriter<gutenrip::worker::RenderPlaneStreamResponse,
                             gutenrip::worker::RenderPlaneStreamRequest>* stream)
{
    (void)context;

    auto sendDone = [&](bool ok, const std::string& message, std::uint64_t bytesWritten) {
        gutenrip::worker::RenderPlaneStreamResponse response;
        auto* done = response.mutable_done();
        done->set_ok(ok);
        done->set_message(message);
        done->set_bytes_written(bytesWritten);
        stream->Write(response);
    };

    gutenrip::worker::RenderPlaneStreamRequest first;
    if (!stream->Read(&first) || !first.has_start()) {
        sendDone(false, "Streaming render did not receive a start message.", 0);
        return grpc::Status::OK;
    }

    const auto& start = first.start();
    gutenrip::worker::RenderPlaneRequest request;
    request.set_job_id(start.job_id());
    request.set_plane(start.plane());
    request.set_half(start.half());
    request.set_printer_driver(start.printer_driver());
    request.set_ink_name(start.ink_name());
    request.set_page_width_points(start.page_width_points());
    request.set_page_height_points(start.page_height_points());
    request.set_dpi_x(start.dpi_x());
    request.set_dpi_y(start.dpi_y());
    request.set_density(start.density());
    request.set_page_width_mm(start.page_width_mm());
    request.set_page_height_mm(start.page_height_mm());
    request.set_image_x_mm(start.image_x_mm());
    request.set_image_y_mm(start.image_y_mm());
    request.set_image_width_mm(start.image_width_mm());
    request.set_image_height_mm(start.image_height_mm());
    *request.mutable_print_settings() = start.print_settings();

    const bool isWhite = request.plane() == gutenrip::worker::PLANE_WHITE;
    const std::string colorInkType = normalizedRawInkTypeFor(stringSetting(&request, "InkType", "Physical6"));
    const std::string whiteInkType = normalizedRawInkTypeFor(stringSetting(&request, "WhiteInkType",
                                                   defaultWhiteInkTypeFor(colorInkType)));
    const std::string inkType = isWhite ? whiteInkType : colorInkType;
    const std::string requestedWhitePrintingMode = stringSetting(&request, "WhitePrintingMode", "BW");
    const std::string printingMode = isWhite
        ? std::string("Color")
        : stringSetting(&request, "ColorPrintingMode", "Color");
    const std::string requestedInputImageType = stringSetting(&request, "InputImageType", "Auto");
    const std::string requestedPrinterDriver = request.printer_driver().empty()
        ? defaultCatalogDriver()
        : request.printer_driver();
    const std::string printerDriver = requestedPrinterDriver;
    const stp_printer_t* printer = stp_get_printer_by_driver(printerDriver.c_str());
    if (!printer) {
        sendDone(false, "Printer driver not found: " + printerDriver, 0);
        return grpc::Status::OK;
    }

    stp_vars_t* vars = stp_vars_create_copy(stp_printer_get_defaults(printer));
    if (!vars) {
        sendDone(false, "Unable to create printer vars.", 0);
        return grpc::Status::OK;
    }

    LayoutImageContext layoutContext = layoutFromRequest(&request, isWhite);
    layoutContext.hasSourceImage = true;
    layoutContext.sourceWidth = std::max(1, start.image_width_px());
    layoutContext.sourceHeight = std::max(1, start.image_height_px());
    layoutContext.imageWidth = layoutContext.sourceWidth;
    layoutContext.imageHeight = layoutContext.sourceHeight;
    layoutContext.rawChannelCount = static_cast<std::size_t>(std::max(1, start.raw_channels()));
    layoutContext.sourceInputImageType = "RawStream";
    layoutContext.stream = stream;

    if (layoutContext.rawChannelCount != rawChannelCountForInkType(inkType)) {
        stp_vars_destroy(vars);
        sendDone(false,
                 "Raw channel count does not match selected InkType=" + inkType +
                     ": got " + std::to_string(layoutContext.rawChannelCount) +
                     ", expected " + std::to_string(rawChannelCountForInkType(inkType)),
                 0);
        return grpc::Status::OK;
    }

    const std::string inputImageType = "Raw";
    const std::string requestedMediaType = stringSetting(&request, "MediaType", "Plain");
    const std::string inkSet = isLayeredRawDriver(printerDriver)
        ? rawInkSetForInkType(inkType)
        : stringSetting(&request, "InkSet", "None");
    const std::string mediaType = rawMediaTypeForDriver(printerDriver, requestedMediaType, inkSet);
    const std::string ditherAlgorithm = rawDitherAlgorithmForDriver(printerDriver, isWhite, &request);
    const std::string resolution = stringSetting(&request, "Resolution", "1440x1440ov");
    const double rawDensity = boundedDoubleSetting(&request,
                                                   "Density",
                                                   request.density() > 0.0 ? request.density() : 1.0,
                                                   0.0,
                                                   1.0);
    const double outputInkLimit = boundedDoubleSetting(
        &request,
        "OutputInkLimit",
        doubleSetting(&request, "InkLimit", neutralRawInkLimitForChannelCount(layoutContext.rawChannelCount)),
        0.01,
        64.0);
    const bool rawHasColorInk = false;
    const bool rawHasWhiteInk = false;
    const double rawInkLimit = outputInkLimit;
    const auto channelDensities = rawChannelDensities(&request, layoutContext.rawChannelCount);
    const PrintedDpi printedDpi = activePrintedDpiForResolution(vars, resolution);
    const double rawEscIFeatherEdge =
        escIFeatherEdgeForResolution(
            boundedDoubleSetting(&request, "EscIFeatherEdge", 1.0, 0.0, 1.0),
            printedDpi);
    int bandEnhancement = intSetting(&request, "BandEnhancement", 0);
    const std::string colorCorrection = stringSetting(&request, "ColorCorrection", "Density");
    setStringParameter(vars, "PageSize", pageSizeForLayout(&request));
    setStringParameter(vars, "InputSlot", stringSetting(&request, "InputSlot", "Rear"));
    setStringParameter(vars, "Duplex", stringSetting(&request, "Duplex", "None"));
    setStringParameter(vars, "CDInnerRadius", stringSetting(&request, "CDInnerRadius", "None"));
    setStringParameter(vars, "MediaType", mediaType);
    setStringParameter(vars, "Resolution", resolution);
    setStringParameter(vars, "Quality", stringSetting(&request, "Quality", "Standard"));
    setStringParameter(vars, "PrintingMode", printingMode);
    setStringParameter(vars, "InputImageType", inputImageType);
    setStringParameter(vars, "RawChannels", std::to_string(layoutContext.rawChannelCount));
    setStringParameter(vars, "InkSet", inkSet);
    setStringParameter(vars, "InkType", inkType);
    setStringParameter(vars, "ColorCorrection", colorCorrection);
    setStringParameter(vars, "DitherAlgorithm", ditherAlgorithm);
    setStringParameter(vars, "ChannelBitDepth", stringSetting(&request, "ChannelBitDepth", "16"));
    setStringParameter(vars, "PrintingDirection", stringSetting(&request, "PrintingDirection", "Bidirectional"));
    setStringParameter(vars, "Weave", stringSetting(&request, "Weave", "None"));
    setIntParameter(vars, "BandEnhancement", bandEnhancement);
    setFloatParameter(vars, "Density", rawDensity);
    setFloatParameter(vars, "InkLimit", rawInkLimit);
    setFloatParameter(vars, "EscIFeatherEdge", rawEscIFeatherEdge);
    setFloatParameter(vars, "Brightness", doubleSetting(&request, "Brightness", 1.0));
    setFloatParameter(vars, "Contrast", doubleSetting(&request, "Contrast", 1.0));
    setFloatParameter(vars, "Saturation", doubleSetting(&request, "Saturation", 1.0));
    setFloatParameter(vars, "Gamma", doubleSetting(&request, "Gamma", 1.0));

    const ResolvedChannelLayout channelLayout = resolveChannelLayout(
        vars,
        printerDriver,
        resolution,
        start.compose(),
        channelDensities);
    if (!channelLayout.ok) {
        stp_vars_destroy(vars);
        sendDone(false, channelLayout.error, 0);
        return grpc::Status::OK;
    }
    applyChannelLayout(vars, channelLayout);

    const stp_dimension_t pageWidth = mmToPoints(pageMMFromRequest(request.page_width_mm(),
                                                                   request.page_width_points(),
                                                                   210.0));
    const stp_dimension_t pageHeight = mmToPoints(pageMMFromRequest(request.page_height_mm(),
                                                                    request.page_height_points(),
                                                                    297.0));
    constexpr stp_dimension_t minMargin = 0.0;
    stp_set_page_width(vars, pageWidth);
    stp_set_page_height(vars, pageHeight);
    stp_set_left(vars, std::max<stp_dimension_t>(minMargin, mmToPoints(layoutContext.imageXMM)));
    stp_set_top(vars, std::max<stp_dimension_t>(minMargin, mmToPoints(layoutContext.imageYMM)));
    stp_set_width(vars, std::max<stp_dimension_t>(1.0, mmToPoints(layoutContext.imageWidthMM)));
    stp_set_height(vars, std::max<stp_dimension_t>(1.0, mmToPoints(layoutContext.imageHeightMM)));

    std::unique_ptr<yink::rip::ChannelOffsetCorrector> offsetCorrector;
    if (channelLayout.offsetCorrectionEnabled) {
        offsetCorrector = std::make_unique<yink::rip::ChannelOffsetCorrector>(
            correctionOptionsForLayout(start.compose(), channelLayout));
    }
    OutputContext outputCtx;
    outputCtx.stream = stream;
    outputCtx.offsetCorrector = offsetCorrector.get();
    stp_set_outfunc(vars, outputFunc);
    stp_set_outdata(vars, &outputCtx);

    stp_image_t image;
    std::memset(&image, 0, sizeof(image));
    image.width = imageWidthFunc;
    image.height = imageHeightFunc;
    image.get_row = getRowFunc;
    image.get_appname = appNameFunc;
    image.rep = &layoutContext;

    std::ostringstream streamBeginLog;
    streamBeginLog << "[guten-rip-worker] RenderPlaneStream begin"
                   << " job_id=" << request.job_id()
                   << " plane=" << (isWhite ? "white" : "color")
                   << " printer=" << printerDriver
                   << " selectedInkType=" << inkType
                   << " imagePx=" << layoutContext.imageWidth << "x" << layoutContext.imageHeight
                   << " rawChannels=" << layoutContext.rawChannelCount
                   << " ditherAlgorithm=" << ditherAlgorithm
                   << " density=" << rawDensity
                   << " inkLimit=" << rawInkLimit
                   << " rawChannelDensities=" << rawChannelDensitySummary(channelDensities)
                   << " activeNozzles=" << channelLayout.activeNozzles
                   << " runtimeGeometry="
                   << (channelLayout.runtimeGeometryEnabled ? "true" : "false")
                   << " channelOffsetCorrection="
                   << (channelLayout.offsetCorrectionEnabled ? "true" : "false")
                   << " escIFeatherEdge=" << rawEscIFeatherEdge
                   << " bandEnhancement=" << bandEnhancement
                   << " rawHasColorInk=" << (rawHasColorInk ? "true" : "false")
                   << " rawHasWhiteInk=" << (rawHasWhiteInk ? "true" : "false")
                   << " requestedInputImageType=" << requestedInputImageType
                   << " STP_DATA_PATH=" << (std::getenv("STP_DATA_PATH") ? std::getenv("STP_DATA_PATH") : "");
    const std::string streamBeginMessage = streamBeginLog.str();
    std::cerr << streamBeginMessage << std::endl;
    appendWorkerRuntimeLog(streamBeginMessage);

    const int printResult = stp_print(vars, &image);
    if (offsetCorrector) {
        const auto tail = offsetCorrector->finish();
        (void)writeOutputBytes(&outputCtx, tail);
    }

    std::ostringstream streamDoneLog;
    streamDoneLog << "[guten-rip-worker] RenderPlaneStream stp_print returned"
                  << " job_id=" << request.job_id()
                  << " result=" << printResult
                  << " bytes=" << outputCtx.bytesWritten
                  << " streamError=" << layoutContext.streamError;
    const std::string streamDoneMessage = streamDoneLog.str();
    std::cerr << streamDoneMessage << std::endl;
    appendWorkerRuntimeLog(streamDoneMessage);

    stp_vars_destroy(vars);

    if (!layoutContext.streamError.empty()) {
        sendDone(false, layoutContext.streamError, outputCtx.bytesWritten);
        return grpc::Status::OK;
    }
    if (outputCtx.writeFailed) {
        sendDone(false, "Unable to write streaming PRN output.", outputCtx.bytesWritten);
        return grpc::Status::OK;
    }
    if (printResult != 1 || outputCtx.bytesWritten == 0) {
        sendDone(false,
                 "Gutenprint streaming render failed; synthetic fallback is disabled for PRN output. stp_print=" +
                     std::to_string(printResult) + ", bytes=" +
                     std::to_string(outputCtx.bytesWritten),
                 outputCtx.bytesWritten);
        return grpc::Status::OK;
    }

    sendDone(true,
             "Rendered streaming raw rows via libgutenprint. InputImageType=" + inputImageType +
                 ", PrinterDriver=" + printerDriver +
                 ", MediaType=" + mediaType +
                 ", InkSet=" + inkSet +
                 ", RawChannels=" + std::to_string(layoutContext.rawChannelCount) +
                 ", PrintingMode=" + printingMode +
                 ", DitherAlgorithm=" + ditherAlgorithm +
                 ", ColorCorrection=" + colorCorrection +
                 ", Density=" + std::to_string(rawDensity) +
                 ", InkLimit=" + std::to_string(rawInkLimit) +
                 ", OutputInkLimit=" + std::to_string(outputInkLimit) +
                 ", RawChannelDensities=" + rawChannelDensitySummary(channelDensities) +
                 ", ActiveNozzles=" + std::to_string(channelLayout.activeNozzles) +
                 ", RuntimeGeometry=" +
                     (channelLayout.runtimeGeometryEnabled ? "true" : "false") +
                 ", ChannelOffsetCorrection=" +
                     (channelLayout.offsetCorrectionEnabled ? "true" : "false") +
                 ", EscIFeatherEdge=" + std::to_string(rawEscIFeatherEdge) +
                 ", source=" + layoutContext.sourceInputImageType + ".",
             outputCtx.bytesWritten);
    return grpc::Status::OK;
}

grpc::Status RipWorkerService::CancelJob(grpc::ServerContext* context,
                                         const gutenrip::worker::CancelJobRequest* request,
                                         gutenrip::worker::CancelJobResult* response)
{
    (void)context;
    response->set_ok(true);
    response->set_message("Cancel requested for job: " + request->job_id());
    return grpc::Status::OK;
}
