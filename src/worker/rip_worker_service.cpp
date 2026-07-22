#include "rip_worker_service.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
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

constexpr std::uint32_t kProtocolVersion = 2;
constexpr int kMaximumRuntimeOffset = 359;
constexpr int kMaximumRawChannels = 32;

using WorkerStream = grpc::ServerReaderWriter<gutenrip::worker::RenderPlaneStreamResponse,
                                               gutenrip::worker::RenderPlaneStreamRequest>;

struct OutputContext {
    WorkerStream* stream = nullptr;
    std::uint64_t rawBytesWritten = 0;
    bool writeFailed = false;
};

struct StreamImageContext {
    WorkerStream* stream = nullptr;
    int width = 0;
    int height = 0;
    int channels = 0;
    std::string error;
};

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
           text.find("escp2-et2750") != std::string::npos &&
           text.find("model=\"901\"") != std::string::npos &&
           text.find("escp2-et8550") != std::string::npos;
}

std::filesystem::path executableDir()
{
#ifdef _WIN32
    wchar_t buffer[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        return std::filesystem::path(buffer).parent_path();
    }
#endif
    return std::filesystem::current_path();
}

void configurePrintEngineDataPath()
{
    if (envIsSet("STP_DATA_PATH") &&
        xmlDataPathContainsLocalDrivers(std::getenv("STP_DATA_PATH"))) {
        return;
    }

    const std::filesystem::path current = std::filesystem::current_path();
    const std::filesystem::path executable = executableDir();
    const std::filesystem::path candidates[] = {
        current.parent_path() / "xml",
        executable.parent_path() / "xml",
        current / "xml",
        executable / "xml",
        current / "gutenprint" / "src" / "xml",
        current.parent_path() / "gutenprint" / "src" / "xml",
        current / "worker" / "gutenprint" / "src" / "xml",
        current.parent_path() / "worker" / "gutenprint" / "src" / "xml",
        executable / "gutenprint" / "src" / "xml",
        executable.parent_path() / "gutenprint" / "src" / "xml",
        executable / "worker" / "gutenprint" / "src" / "xml",
        executable.parent_path() / "worker" / "gutenprint" / "src" / "xml",
    };

    for (const auto& candidate : candidates) {
        if (xmlDataPathContainsLocalDrivers(candidate)) {
            setEnv("STP_DATA_PATH", candidate);
            std::cout << "STP_DATA_PATH=" << candidate.string() << '\n';
            return;
        }
    }
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
                              const res_t* resolution,
                              const char* parameter)
{
    if (resolution && resolution->v &&
        stp_check_int_parameter(resolution->v, parameter, STP_PARAMETER_ACTIVE)) {
        return stp_get_int_parameter(resolution->v, parameter);
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
    if (!vars || !option || !option->name || !profile ||
        std::strcmp(option->name, "None") == 0) {
        return;
    }

    stp_set_string_parameter(vars, "Resolution", option->name);
    const res_t* resolution = stpi_escp2_find_resolution(vars);
    if (!resolution) {
        return;
    }

    auto* output = profile->add_resolutions();
    output->set_name(option->name);
    output->set_label(option->text && option->text[0] != '\0' ? option->text : option->name);
    output->set_physical_dpi_x(resolution->hres);
    output->set_physical_dpi_y(resolution->vres);
    output->set_printed_dpi_x(resolution->printed_hres > 0
                                  ? resolution->printed_hres
                                  : resolution->hres);
    output->set_printed_dpi_y(resolution->printed_vres > 0
                                  ? resolution->printed_vres
                                  : resolution->vres);
    output->set_base_dpi_x(intParameterForResolution(vars, resolution, "escp2_base_res"));
}

void fillResolutions(stp_vars_t* vars, gutenrip::worker::PrintDriverProfile* profile)
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

const inkname_t* physicalInkName(stp_vars_t* vars)
{
    const inklist_t* inkList = stpi_escp2_inklist(vars);
    if (!inkList) {
        return nullptr;
    }
    for (unsigned int i = 0; i < inkList->n_inks; ++i) {
        if (inkList->inknames[i].name &&
            std::strncmp(inkList->inknames[i].name, "Physical", 8) == 0) {
            return &inkList->inknames[i];
        }
    }
    return nullptr;
}

void addCatalogDriver(const std::string& driverId,
                      gutenrip::worker::PrintDriverCatalog* response)
{
    const stp_printer_t* printer = stp_get_printer_by_driver(driverId.c_str());
    if (!printer) {
        response->add_warnings("Print driver not found: " + driverId);
        return;
    }

    const char* family = stp_printer_get_family(printer);
    if (!family || std::strcmp(family, "escp2") != 0) {
        response->add_warnings("Print driver is not an ESC/P2 profile: " + driverId);
        return;
    }

    stp_vars_t* vars = stp_vars_create_copy(stp_printer_get_defaults(printer));
    if (!vars) {
        response->add_warnings("Unable to create print engine settings for: " + driverId);
        return;
    }
    primeCatalogVars(vars);

    const stpi_escp2_printer_t* escp2 = stpi_escp2_get_printer(vars);
    const inkname_t* inkName = physicalInkName(vars);
    if (!escp2 || !inkName) {
        response->add_warnings("Unable to read raw print profile for: " + driverId);
        stp_vars_destroy(vars);
        return;
    }

    stp_set_string_parameter(vars,
                             "RawChannels",
                             std::to_string(inkName->channel_count).c_str());
    stp_set_string_parameter(vars, "InkType", inkName->name);

    auto* profile = response->add_drivers();
    profile->set_printer_label(stp_printer_get_long_name(printer)
                                   ? stp_printer_get_long_name(printer)
                                   : driverId);
    profile->set_driver(driverId);
    profile->set_model_id(stp_printer_get_model(printer));
    profile->set_nozzles(escp2->nozzles);
    profile->set_nozzle_separation(escp2->nozzle_separation);
    profile->set_base_separation(escp2->base_separation);

    int physicalIndex = 0;
    for (unsigned int i = 0; i < inkName->channel_count; ++i) {
        const ink_channel_t& channel = inkName->channels[i];
        for (unsigned int j = 0; j < channel.n_subchannels; ++j) {
            const physical_subchannel_t& subchannel = channel.subchannels[j];
            auto* physical = profile->add_physical_channels();
            physical->set_index(physicalIndex++);
            physical->set_esc_color(escIColorForSubchannel(subchannel));
            physical->set_physical_head_offset(subchannel.head_offset);
        }
    }
    profile->set_channel_count(physicalIndex);
    fillResolutions(vars, profile);
    stp_vars_destroy(vars);
}

bool finite(double value)
{
    return std::isfinite(value);
}

bool validateGeometry(const gutenrip::worker::PageGeometryPoints& geometry,
                      std::string* error)
{
    const double values[] = {
        geometry.page_width(), geometry.page_height(), geometry.left(),
        geometry.top(), geometry.print_width(), geometry.print_height(),
    };
    for (const double value : values) {
        if (!finite(value)) {
            if (error) *error = "Page geometry contains a non-finite value.";
            return false;
        }
    }
    if (geometry.page_width() <= 0.0 || geometry.page_height() <= 0.0 ||
        geometry.print_width() <= 0.0 || geometry.print_height() <= 0.0 ||
        geometry.left() < 0.0 || geometry.top() < 0.0 ||
        geometry.left() + geometry.print_width() > geometry.page_width() + 0.001 ||
        geometry.top() + geometry.print_height() > geometry.page_height() + 0.001) {
        if (error) *error = "Page geometry is outside the page bounds.";
        return false;
    }
    return true;
}

bool validateSettings(const gutenrip::worker::PrintEngineSettings& settings,
                      int channelCount,
                      std::string* error)
{
    const std::string* requiredStrings[] = {
        &settings.page_size(), &settings.input_slot(), &settings.duplex(),
        &settings.cd_inner_radius(), &settings.media_type(), &settings.resolution(),
        &settings.quality(), &settings.printing_mode(), &settings.input_image_type(),
        &settings.ink_set(), &settings.ink_type(), &settings.color_correction(),
        &settings.dither_algorithm(), &settings.channel_bit_depth(),
        &settings.printing_direction(), &settings.weave(),
    };
    for (const std::string* value : requiredStrings) {
        if (!value || value->empty()) {
            if (error) *error = "Print engine settings contain an empty required string.";
            return false;
        }
    }

    const double finiteValues[] = {
        settings.density(), settings.ink_limit(), settings.esc_i_feather_edge(),
        settings.brightness(), settings.contrast(), settings.saturation(), settings.gamma(),
    };
    for (const double value : finiteValues) {
        if (!finite(value)) {
            if (error) *error = "Print engine settings contain a non-finite value.";
            return false;
        }
    }
    if (settings.density() < 0.0 || settings.density() > 1.0 ||
        settings.ink_limit() <= 0.0 || settings.ink_limit() > 64.0 ||
        settings.esc_i_feather_edge() < 0.0 || settings.esc_i_feather_edge() > 1.0) {
        if (error) *error = "Print engine settings are outside supported bounds.";
        return false;
    }
    if (settings.has_active_nozzles() && settings.active_nozzles() <= 0) {
        if (error) *error = "Active nozzle count must be positive when present.";
        return false;
    }
    if (settings.raw_channels_size() != channelCount) {
        if (error) *error = "Raw channel runtime count does not match the image channel count.";
        return false;
    }

    std::vector<bool> seen(static_cast<std::size_t>(channelCount), false);
    bool anyGeometry = false;
    bool allGeometry = true;
    for (const auto& channel : settings.raw_channels()) {
        if (channel.index() < 0 || channel.index() >= channelCount ||
            seen[static_cast<std::size_t>(channel.index())]) {
            if (error) *error = "Raw channel runtime indices are invalid or duplicated.";
            return false;
        }
        seen[static_cast<std::size_t>(channel.index())] = true;
        if (!finite(channel.density()) || channel.density() < 0.0 || channel.density() > 1.0) {
            if (error) *error = "Raw channel density is outside supported bounds.";
            return false;
        }
        const bool hasGeometry = channel.has_head_offset() && channel.has_correction_rows();
        anyGeometry = anyGeometry || hasGeometry;
        allGeometry = allGeometry && hasGeometry;
        if (channel.has_head_offset() != channel.has_correction_rows() ||
            (channel.has_head_offset() &&
             (channel.head_offset() < 0 || channel.head_offset() > kMaximumRuntimeOffset ||
              channel.correction_rows() < 0 ||
              channel.correction_rows() > kMaximumRuntimeOffset))) {
            if (error) *error = "Raw channel runtime geometry is incomplete or out of range.";
            return false;
        }
    }
    if (anyGeometry != allGeometry || anyGeometry != settings.has_active_nozzles()) {
        if (error) *error = "Runtime nozzle geometry must be supplied for every channel or none.";
        return false;
    }
    return true;
}

bool validateStart(const gutenrip::worker::RenderPlaneStreamStart& start,
                   std::string* error)
{
    if (start.job_id().empty() || start.printer_driver().empty()) {
        if (error) *error = "Streaming render requires a job id and printer driver.";
        return false;
    }
    if (!start.has_geometry() || !validateGeometry(start.geometry(), error)) {
        return false;
    }
    if (!start.has_image() || start.image().width() <= 0 || start.image().height() <= 0 ||
        start.image().channels() <= 0 || start.image().channels() > kMaximumRawChannels) {
        if (error) *error = "Streaming RAW image shape is invalid.";
        return false;
    }
    if (!start.has_settings()) {
        if (error) *error = "Streaming render is missing print engine settings.";
        return false;
    }
    return validateSettings(start.settings(), start.image().channels(), error);
}

void setStringParameter(stp_vars_t* vars, const char* key, const std::string& value)
{
    stp_set_string_parameter(vars, key, value.c_str());
}

void applySettings(stp_vars_t* vars,
                   const gutenrip::worker::PrintEngineSettings& settings,
                   int rawChannels)
{
    setStringParameter(vars, "PageSize", settings.page_size());
    setStringParameter(vars, "InputSlot", settings.input_slot());
    setStringParameter(vars, "Duplex", settings.duplex());
    setStringParameter(vars, "CDInnerRadius", settings.cd_inner_radius());
    setStringParameter(vars, "MediaType", settings.media_type());
    setStringParameter(vars, "Resolution", settings.resolution());
    setStringParameter(vars, "Quality", settings.quality());
    setStringParameter(vars, "PrintingMode", settings.printing_mode());
    setStringParameter(vars, "InputImageType", settings.input_image_type());
    setStringParameter(vars, "RawChannels", std::to_string(rawChannels));
    setStringParameter(vars, "InkSet", settings.ink_set());
    setStringParameter(vars, "InkType", settings.ink_type());
    setStringParameter(vars, "ColorCorrection", settings.color_correction());
    setStringParameter(vars, "DitherAlgorithm", settings.dither_algorithm());
    setStringParameter(vars, "ChannelBitDepth", settings.channel_bit_depth());
    setStringParameter(vars, "PrintingDirection", settings.printing_direction());
    setStringParameter(vars, "Weave", settings.weave());
    stp_set_int_parameter(vars, "BandEnhancement", settings.band_enhancement());
    stp_set_float_parameter(vars, "Density", settings.density());
    stp_set_float_parameter(vars, "InkLimit", settings.ink_limit());
    stp_set_float_parameter(vars, "EscIFeatherEdge", settings.esc_i_feather_edge());
    stp_set_float_parameter(vars, "Brightness", settings.brightness());
    stp_set_float_parameter(vars, "Contrast", settings.contrast());
    stp_set_float_parameter(vars, "Saturation", settings.saturation());
    stp_set_float_parameter(vars, "Gamma", settings.gamma());

    if (settings.has_active_nozzles()) {
        stp_set_int_parameter(vars, "RawActiveNozzles", settings.active_nozzles());
    }
    for (const auto& channel : settings.raw_channels()) {
        const std::string suffix = std::to_string(channel.index());
        stp_set_float_parameter(vars,
                                ("RawChannelDensity" + suffix).c_str(),
                                channel.density());
        if (channel.has_head_offset()) {
            stp_set_int_parameter(vars,
                                  ("RawHeadOffset" + suffix).c_str(),
                                  channel.head_offset());
            stp_set_int_parameter(vars,
                                  ("RawChannelCorrectionRows" + suffix).c_str(),
                                  channel.correction_rows());
        }
    }
}

bool writeRawPrn(OutputContext* context, const char* buffer, std::size_t bytes)
{
    if (!context || !context->stream || !buffer || bytes == 0) {
        return bytes == 0;
    }
    gutenrip::worker::RenderPlaneStreamResponse response;
    auto* chunk = response.mutable_raw_prn_chunk();
    chunk->set_data(buffer, bytes);
    chunk->set_raw_prn_bytes_written(context->rawBytesWritten + bytes);
    if (!context->stream->Write(response)) {
        context->writeFailed = true;
        return false;
    }
    context->rawBytesWritten += bytes;
    return true;
}

void outputFunc(void* data, const char* buffer, std::size_t bytes)
{
    auto* context = static_cast<OutputContext*>(data);
    if (!context || context->writeFailed) {
        return;
    }
    (void)writeRawPrn(context, buffer, bytes);
}

bool readRawRow(StreamImageContext* context,
                unsigned char* data,
                std::size_t byteLimit,
                int row)
{
    std::memset(data, 0, byteLimit);
    if (!context || !context->stream || row < 0 || row >= context->height) {
        if (context) context->error = "Print engine requested a RAW row outside the image.";
        return false;
    }

    gutenrip::worker::RenderPlaneStreamResponse request;
    auto* rowRequest = request.mutable_raw_row_request();
    rowRequest->set_row(row);
    rowRequest->set_byte_limit(byteLimit);
    if (!context->stream->Write(request)) {
        context->error = "Unable to request a streaming RAW row.";
        return false;
    }

    gutenrip::worker::RenderPlaneStreamRequest input;
    if (!context->stream->Read(&input) || !input.has_raw_row()) {
        context->error = "Streaming RAW row response was missing.";
        return false;
    }
    if (input.raw_row().row() != row) {
        context->error = "Streaming RAW row response did not match the requested row.";
        return false;
    }
    if (input.raw_row().data().size() != byteLimit) {
        context->error = "Streaming RAW row byte count did not match the print engine request.";
        return false;
    }
    if (byteLimit > 0) {
        std::memcpy(data, input.raw_row().data().data(), byteLimit);
    }
    return true;
}

stp_image_status_t getRowFunc(stp_image_t* image,
                              unsigned char* data,
                              std::size_t byteLimit,
                              int row)
{
    auto* context = image ? static_cast<StreamImageContext*>(image->rep) : nullptr;
    if (!readRawRow(context, data, byteLimit, row)) {
        return STP_IMAGE_STATUS_ABORT;
    }
    return STP_IMAGE_STATUS_OK;
}

int imageWidthFunc(stp_image_t* image)
{
    const auto* context = image ? static_cast<const StreamImageContext*>(image->rep) : nullptr;
    return context ? context->width : 1;
}

int imageHeightFunc(stp_image_t* image)
{
    const auto* context = image ? static_cast<const StreamImageContext*>(image->rep) : nullptr;
    return context ? context->height : 1;
}

const char* appNameFunc(stp_image_t*)
{
    return "guten-rip-worker";
}

} // namespace

RipWorkerService::RipWorkerService()
{
    configurePrintEngineDataPath();
    stp_init();

    if (stp_printer_model_count() == 0) {
        std::cerr << "[guten-rip-worker] WARNING: print engine initialized without driver models."
                  << " STP_DATA_PATH="
                  << (std::getenv("STP_DATA_PATH") ? std::getenv("STP_DATA_PATH") : "(not set)")
                  << std::endl;
    }
}

RipWorkerService::~RipWorkerService() = default;

grpc::Status RipWorkerService::GetCapabilities(grpc::ServerContext* context,
                                               const gutenrip::worker::Empty* request,
                                               gutenrip::worker::Capabilities* response)
{
    (void)context;
    (void)request;
    response->set_worker_name("guten-rip-worker");
    response->set_protocol_version(kProtocolVersion);
    response->set_print_engine_ready(stp_printer_model_count() > 0);
    return grpc::Status::OK;
}

grpc::Status RipWorkerService::GetPrintDriverCatalog(
    grpc::ServerContext* context,
    const gutenrip::worker::PrintDriverCatalogRequest* request,
    gutenrip::worker::PrintDriverCatalog* response)
{
    (void)context;
    if (!request || request->driver_ids_size() == 0) {
        response->add_warnings("No print driver ids were requested.");
        return grpc::Status::OK;
    }

    std::set<std::string> seen;
    for (const std::string& driverId : request->driver_ids()) {
        if (driverId.empty() || !seen.insert(driverId).second) {
            response->add_warnings("Print driver id is empty or duplicated.");
            continue;
        }
        addCatalogDriver(driverId, response);
    }
    return grpc::Status::OK;
}

grpc::Status RipWorkerService::RenderPlaneStream(
    grpc::ServerContext* context,
    WorkerStream* stream)
{
    auto sendDone = [&](bool ok, const std::string& message, std::uint64_t rawBytesWritten) {
        gutenrip::worker::RenderPlaneStreamResponse response;
        auto* done = response.mutable_done();
        done->set_ok(ok);
        done->set_message(message);
        done->set_raw_prn_bytes_written(rawBytesWritten);
        (void)stream->Write(response);
    };

    gutenrip::worker::RenderPlaneStreamRequest first;
    if (!stream->Read(&first) || !first.has_start()) {
        sendDone(false, "Streaming render did not receive a start message.", 0);
        return grpc::Status::OK;
    }

    const auto& start = first.start();
    std::string validationError;
    if (!validateStart(start, &validationError)) {
        sendDone(false, validationError, 0);
        return grpc::Status::OK;
    }

    const stp_printer_t* printer = stp_get_printer_by_driver(start.printer_driver().c_str());
    if (!printer) {
        sendDone(false, "Print driver not found: " + start.printer_driver(), 0);
        return grpc::Status::OK;
    }

    stp_vars_t* vars = stp_vars_create_copy(stp_printer_get_defaults(printer));
    if (!vars) {
        sendDone(false, "Unable to create print engine settings.", 0);
        return grpc::Status::OK;
    }

    applySettings(vars, start.settings(), start.image().channels());
    const auto& geometry = start.geometry();
    stp_set_page_width(vars, geometry.page_width());
    stp_set_page_height(vars, geometry.page_height());
    stp_set_left(vars, geometry.left());
    stp_set_top(vars, geometry.top());
    stp_set_width(vars, geometry.print_width());
    stp_set_height(vars, geometry.print_height());

    if (stp_verify(vars) != 1) {
        stp_vars_destroy(vars);
        sendDone(false, "Print engine rejected the supplied render settings.", 0);
        return grpc::Status::OK;
    }

    OutputContext outputContext;
    outputContext.stream = stream;
    stp_set_outfunc(vars, outputFunc);
    stp_set_outdata(vars, &outputContext);

    StreamImageContext imageContext;
    imageContext.stream = stream;
    imageContext.width = start.image().width();
    imageContext.height = start.image().height();
    imageContext.channels = start.image().channels();

    stp_image_t image;
    std::memset(&image, 0, sizeof(image));
    image.width = imageWidthFunc;
    image.height = imageHeightFunc;
    image.get_row = getRowFunc;
    image.get_appname = appNameFunc;
    image.rep = &imageContext;

    std::ostringstream beginLog;
    beginLog << "[guten-rip-worker] RenderPlaneStream begin"
             << " job_id=" << start.job_id()
             << " driver=" << start.printer_driver()
             << " imagePx=" << imageContext.width << 'x' << imageContext.height
             << " rawChannels=" << imageContext.channels
             << " resolution=" << start.settings().resolution()
             << " inkLimit=" << start.settings().ink_limit()
             << " runtimeGeometry=" << (start.settings().has_active_nozzles() ? "true" : "false")
             << " STP_DATA_PATH="
             << (std::getenv("STP_DATA_PATH") ? std::getenv("STP_DATA_PATH") : "");
    std::cerr << beginLog.str() << std::endl;
    appendWorkerRuntimeLog(beginLog.str());

    const int printResult = stp_print(vars, &image);
    stp_vars_destroy(vars);

    std::ostringstream doneLog;
    doneLog << "[guten-rip-worker] RenderPlaneStream finished"
            << " job_id=" << start.job_id()
            << " result=" << printResult
            << " rawBytes=" << outputContext.rawBytesWritten
            << " streamError=" << imageContext.error;
    std::cerr << doneLog.str() << std::endl;
    appendWorkerRuntimeLog(doneLog.str());

    if (context->IsCancelled()) {
        return grpc::Status::CANCELLED;
    }
    if (!imageContext.error.empty()) {
        sendDone(false, imageContext.error, outputContext.rawBytesWritten);
        return grpc::Status::OK;
    }
    if (outputContext.writeFailed) {
        sendDone(false, "Unable to stream raw PRN output.", outputContext.rawBytesWritten);
        return grpc::Status::OK;
    }
    if (printResult != 1 || outputContext.rawBytesWritten == 0) {
        sendDone(false,
                 "Print engine streaming render failed. result=" + std::to_string(printResult) +
                     ", rawBytes=" + std::to_string(outputContext.rawBytesWritten),
                 outputContext.rawBytesWritten);
        return grpc::Status::OK;
    }

    sendDone(true, "Print engine render completed.", outputContext.rawBytesWritten);
    return grpc::Status::OK;
}
