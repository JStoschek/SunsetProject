#include "PipelineConfig.h"

#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

namespace {

// Trim ASCII whitespace from both ends.
std::string trim(const std::string& s) {
    const auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return "";
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

double require_double(const std::map<std::string, std::string>& kv,
                      const std::string& key) {
    const auto it = kv.find(key);
    if (it == kv.end())
        throw std::runtime_error("PipelineConfig: missing required key '" + key + "'");
    return std::stod(it->second);
}

int require_int(const std::map<std::string, std::string>& kv,
                const std::string& key) {
    const auto it = kv.find(key);
    if (it == kv.end())
        throw std::runtime_error("PipelineConfig: missing required key '" + key + "'");
    return std::stoi(it->second);
}

double optional_double(const std::map<std::string, std::string>& kv,
                       const std::string& key, double fallback) {
    const auto it = kv.find(key);
    return it == kv.end() ? fallback : std::stod(it->second);
}

std::string require_string(const std::map<std::string, std::string>& kv,
                           const std::string& key) {
    const auto it = kv.find(key);
    if (it == kv.end())
        throw std::runtime_error("PipelineConfig: missing required key '" + key + "'");
    return it->second;
}

std::string optional_string(const std::map<std::string, std::string>& kv,
                            const std::string& key, const std::string& fallback) {
    const auto it = kv.find(key);
    return it == kv.end() ? fallback : it->second;
}

}  // namespace

PipelineConfig PipelineConfig::load(const std::string& path) {
    std::ifstream in(path);
    if (!in)
        throw std::runtime_error("PipelineConfig: cannot open '" + path + "'");

    std::map<std::string, std::string> kv;
    std::string line;
    while (std::getline(in, line)) {
        // Strip comments.
        const auto hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        line = trim(line);
        if (line.empty()) continue;

        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;  // not a key=value line
        kv[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }

    PipelineConfig c;
    c.refraction_coefficient_k   = require_double(kv, "refraction_coefficient_k");
    c.earth_radius_m             = require_double(kv, "earth_radius_m");
    c.observer_eye_height_m      = require_double(kv, "observer_eye_height_m");
    c.horizon_reference_offset_m = require_double(kv, "horizon_reference_offset_m");
    c.cell_per_degree            = require_double(kv, "cell_per_degree");
    c.meters_per_degree_lat      = require_double(kv, "meters_per_degree_lat");
    c.march_step_m               = require_double(kv, "march_step_m");
    c.azimuth_min_deg            = require_double(kv, "azimuth_min_deg");
    c.azimuth_max_deg            = require_double(kv, "azimuth_max_deg");
    c.azimuth_step_deg           = require_double(kv, "azimuth_step_deg");
    c.strip_height_deg           = require_double(kv, "strip_height_deg");
    c.strip_tilt_margin_deg      = optional_double(kv, "strip_tilt_margin_deg", 0.65);
    c.coast_march_step_km        = require_double(kv, "coast_march_step_km");
    c.coast_march_max_km         = require_double(kv, "coast_march_max_km");
    c.worker_threads             = require_int(kv, "worker_threads");
    c.dem_lru_capacity           = require_int(kv, "dem_lru_capacity");
    c.ocean_lru_capacity         = require_int(kv, "ocean_lru_capacity");
    c.dem_dir                    = require_string(kv, "dem_dir");
    c.osm_water_polygons_path    = require_string(kv, "osm_water_polygons_path");
    c.osm_inland_water_path      = optional_string(kv, "osm_inland_water_path", "");
    return c;
}
