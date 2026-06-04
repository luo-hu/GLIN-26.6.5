// Export sampled CDF points of Zmin/Zmax for WKT datasets.
//
// This is for GLIN Fig. 5: CDFs of different datasets based on Zmin and Zmax.
// It reads geometries, computes the Z-order range of each geometry MBR using
// the same Encoder as GLIN, sorts Zmin/Zmax separately, then writes sampled CDF
// rows.  Geometry memory is not part of the output; this is a data-distribution
// diagnostic.

#include <limits>

#include "../../glin/Encoder.h"

#include <geos/geom/Envelope.h>
#include <geos/geom/Geometry.h>
#include <geos/geom/GeometryFactory.h>
#include <geos/io/WKTReader.h>
#include <geos/util/GEOSException.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

using Geometry = geos::geom::Geometry;
using GeometryPtr = std::unique_ptr<Geometry>;

struct Options {
  std::string data_file;
  std::string dataset_name = "WKT";
  std::string output_csv;
  std::size_t limit = 1000000;
  std::size_t cdf_points = 1000;
  std::uint64_t seed = 42;
  double cell_xmin = -180.0;
  double cell_ymin = -90.0;
  double cell_size = 0.0000005;
  bool append_csv = false;
};

struct LoadStats {
  std::size_t lines_seen = 0;
  std::size_t parse_errors = 0;
  std::size_t loaded = 0;
  long long load_ns = 0;
  long long compute_ns = 0;
};

void print_usage(const char* program) {
  std::cerr
      << "Usage: " << program << " --data_file /path/to/data.wkt --output_csv out.csv [options]\n"
      << "Options:\n"
      << "  --dataset_name NAME   Dataset label written to CSV/stdout (default: WKT)\n"
      << "  --limit N             Number of valid geometries to load (default: 1000000)\n"
      << "  --cdf_points N        Number of sampled CDF points per Zmin/Zmax (default: 1000)\n"
      << "  --seed N              Recorded in CSV for comparability (default: 42)\n"
      << "  --cell_xmin X         Z-order longitude origin (default: -180)\n"
      << "  --cell_ymin Y         Z-order latitude origin (default: -90)\n"
      << "  --cell_size S         Z-order cell size for x/y (default: 5e-7)\n"
      << "  --append_csv          Append rows to output CSV\n";
}

Options parse_args(int argc, char* argv[]) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    std::string key = argv[i];
    auto require_value = [&](const std::string& flag) -> std::string {
      if (i + 1 >= argc) {
        throw std::runtime_error("Missing value for " + flag);
      }
      return argv[++i];
    };

    if (key == "--data_file") {
      options.data_file = require_value(key);
    } else if (key == "--dataset_name") {
      options.dataset_name = require_value(key);
    } else if (key == "--output_csv") {
      options.output_csv = require_value(key);
    } else if (key == "--limit") {
      options.limit = static_cast<std::size_t>(std::stoull(require_value(key)));
    } else if (key == "--cdf_points") {
      options.cdf_points = static_cast<std::size_t>(std::stoull(require_value(key)));
    } else if (key == "--seed") {
      options.seed = static_cast<std::uint64_t>(std::stoull(require_value(key)));
    } else if (key == "--cell_xmin") {
      options.cell_xmin = std::stod(require_value(key));
    } else if (key == "--cell_ymin") {
      options.cell_ymin = std::stod(require_value(key));
    } else if (key == "--cell_size") {
      options.cell_size = std::stod(require_value(key));
    } else if (key == "--append_csv") {
      options.append_csv = true;
    } else if (key == "--help" || key == "-h") {
      print_usage(argv[0]);
      std::exit(0);
    } else {
      throw std::runtime_error("Unknown option: " + key);
    }
  }

  if (options.data_file.empty()) {
    throw std::runtime_error("--data_file is required");
  }
  if (options.output_csv.empty()) {
    throw std::runtime_error("--output_csv is required");
  }
  if (options.limit == 0) {
    throw std::runtime_error("--limit must be greater than 0");
  }
  if (options.cdf_points == 0) {
    throw std::runtime_error("--cdf_points must be greater than 0");
  }
  return options;
}

template <typename Duration>
long long ns_count(Duration duration) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
}

void strip_bom(std::string& line) {
  if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
      static_cast<unsigned char>(line[1]) == 0xBB &&
      static_cast<unsigned char>(line[2]) == 0xBF) {
    line.erase(0, 3);
  }
}

void strip_cr(std::string& line) {
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
}

std::string extract_balanced_wkt(const std::string& text, std::size_t start) {
  std::size_t open = text.find('(', start);
  if (open == std::string::npos) {
    return text.substr(start);
  }

  int depth = 0;
  for (std::size_t i = open; i < text.size(); ++i) {
    if (text[i] == '(') {
      ++depth;
    } else if (text[i] == ')') {
      --depth;
      if (depth == 0) {
        return text.substr(start, i - start + 1);
      }
    }
  }
  return text.substr(start);
}

std::string extract_wkt(std::string line) {
  strip_bom(line);
  strip_cr(line);
  if (line.empty()) {
    return "";
  }

  if (line.front() == '"') {
    std::size_t close_quote = line.find('"', 1);
    if (close_quote != std::string::npos) {
      return line.substr(1, close_quote - 1);
    }
  }

  static const std::vector<std::string> prefixes = {
      "GEOMETRYCOLLECTION", "MULTIPOLYGON", "POLYGON", "MULTILINESTRING",
      "LINESTRING", "MULTIPOINT", "POINT"};
  for (const auto& prefix : prefixes) {
    std::size_t pos = line.find(prefix);
    if (pos != std::string::npos) {
      return extract_balanced_wkt(line, pos);
    }
  }
  return "";
}

std::vector<GeometryPtr> load_wkt_csv(const Options& options,
                                      geos::io::WKTReader& reader,
                                      LoadStats& stats) {
  std::ifstream input(options.data_file);
  if (!input) {
    throw std::runtime_error("Cannot open data file: " + options.data_file);
  }

  auto start = std::chrono::high_resolution_clock::now();
  std::vector<GeometryPtr> geometries;
  geometries.reserve(options.limit);
  std::string line;
  while (geometries.size() < options.limit && std::getline(input, line)) {
    ++stats.lines_seen;
    std::string wkt = extract_wkt(line);
    if (wkt.empty()) {
      continue;
    }

    try {
      GeometryPtr geometry(reader.read(wkt));
      if (geometry && !geometry->isEmpty()) {
        geometries.push_back(std::move(geometry));
      }
    } catch (const geos::util::GEOSException&) {
      ++stats.parse_errors;
    } catch (const std::exception&) {
      ++stats.parse_errors;
    }
  }
  auto end = std::chrono::high_resolution_clock::now();
  stats.load_ns = ns_count(end - start);
  stats.loaded = geometries.size();
  return geometries;
}

void compute_zranges(const Options& options,
                     const std::vector<GeometryPtr>& geometries,
                     std::vector<std::uint_fast64_t>& zmins,
                     std::vector<std::uint_fast64_t>& zmaxs,
                     LoadStats& stats) {
  zmins.clear();
  zmaxs.clear();
  zmins.reserve(geometries.size());
  zmaxs.reserve(geometries.size());

  Encoder<double> encoder(options.cell_xmin, options.cell_size,
                          options.cell_ymin, options.cell_size);

  auto start = std::chrono::high_resolution_clock::now();
  for (const auto& geometry : geometries) {
    const geos::geom::Envelope* envelope = geometry->getEnvelopeInternal();
    auto range = encoder.encode_z(envelope->getMinX(), envelope->getMinY(),
                                  envelope->getMaxX(), envelope->getMaxY());
    zmins.push_back(range.first);
    zmaxs.push_back(range.second);
  }
  auto end = std::chrono::high_resolution_clock::now();
  stats.compute_ns = ns_count(end - start);
}

bool output_needs_header(const std::string& path, bool append_csv) {
  if (!append_csv) {
    return true;
  }
  std::ifstream existing(path);
  return !existing.good() || existing.peek() == std::ifstream::traits_type::eof();
}

void write_cdf_rows(std::ofstream& output, const Options& options,
                    const LoadStats& stats, const std::string& range_type,
                    std::vector<std::uint_fast64_t> values,
                    std::uint_fast64_t dataset_min,
                    std::uint_fast64_t dataset_max) {
  std::sort(values.begin(), values.end());
  const std::size_t n = values.size();
  if (n == 0) {
    return;
  }

  const std::size_t denominator = std::max<std::size_t>(1, options.cdf_points - 1);
  std::uint_fast64_t previous_rank = std::numeric_limits<std::uint_fast64_t>::max();
  const long double dataset_span =
      dataset_max > dataset_min
          ? static_cast<long double>(dataset_max - dataset_min)
          : 1.0L;
  const long double u64_max =
      static_cast<long double>(std::numeric_limits<std::uint_fast64_t>::max());

  output << std::fixed << std::setprecision(12);
  for (std::size_t point = 0; point < options.cdf_points; ++point) {
    std::size_t rank0 = 0;
    if (options.cdf_points == 1) {
      rank0 = n - 1;
    } else {
      rank0 = (point * (n - 1)) / denominator;
    }
    if (rank0 == previous_rank) {
      continue;
    }
    previous_rank = rank0;

    const std::uint_fast64_t z = values[rank0];
    const long double normalized_dataset =
        static_cast<long double>(z - dataset_min) / dataset_span;
    const long double normalized_u64 = static_cast<long double>(z) / u64_max;
    const long double cdf = static_cast<long double>(rank0 + 1) /
                            static_cast<long double>(n);

    output << options.dataset_name << "," << range_type << ","
           << stats.loaded << "," << options.limit << "," << options.cdf_points
           << "," << options.cell_xmin << "," << options.cell_ymin << ","
           << options.cell_size << "," << options.seed << ","
           << stats.lines_seen << "," << stats.parse_errors << ","
           << stats.load_ns << "," << stats.compute_ns << ","
           << (rank0 + 1) << "," << cdf << ","
           << static_cast<unsigned long long>(z) << ","
           << static_cast<double>(normalized_dataset) << ","
           << static_cast<double>(normalized_u64) << "\n";
  }
}

void write_csv(const Options& options, const LoadStats& stats,
               std::vector<std::uint_fast64_t> zmins,
               std::vector<std::uint_fast64_t> zmaxs) {
  const bool write_header =
      output_needs_header(options.output_csv, options.append_csv);
  std::ofstream output(options.output_csv,
                       options.append_csv ? std::ios::app : std::ios::out);
  if (!output) {
    throw std::runtime_error("Cannot open output CSV: " + options.output_csv);
  }

  if (write_header) {
    output << "dataset,range_type,loaded_count,limit,cdf_points,cell_xmin,"
              "cell_ymin,cell_size,seed,lines_seen,parse_errors,load_ns,"
              "compute_ns,rank,cdf,z_value,z_normalized_dataset,"
              "z_normalized_u64\n";
  }

  auto minmax_min = std::minmax_element(zmins.begin(), zmins.end());
  auto minmax_max = std::minmax_element(zmaxs.begin(), zmaxs.end());
  const std::uint_fast64_t dataset_min =
      std::min(*minmax_min.first, *minmax_max.first);
  const std::uint_fast64_t dataset_max =
      std::max(*minmax_min.second, *minmax_max.second);

  write_cdf_rows(output, options, stats, "Zmin", std::move(zmins), dataset_min,
                 dataset_max);
  write_cdf_rows(output, options, stats, "Zmax", std::move(zmaxs), dataset_min,
                 dataset_max);
}

}  // namespace

int main(int argc, char* argv[]) {
  try {
    Options options = parse_args(argc, argv);

    auto factory = geos::geom::GeometryFactory::create();
    geos::io::WKTReader reader(*factory);

    LoadStats stats;
    std::vector<GeometryPtr> geometries = load_wkt_csv(options, reader, stats);
    if (geometries.empty()) {
      throw std::runtime_error("No valid geometries loaded");
    }

    std::vector<std::uint_fast64_t> zmins;
    std::vector<std::uint_fast64_t> zmaxs;
    compute_zranges(options, geometries, zmins, zmaxs, stats);
    write_csv(options, stats, std::move(zmins), std::move(zmaxs));

    std::cout << std::fixed << std::setprecision(3);
    std::cout << "dataset=" << options.dataset_name
              << " loaded=" << stats.loaded
              << " lines_seen=" << stats.lines_seen
              << " parse_errors=" << stats.parse_errors
              << " load_ms=" << stats.load_ns / 1e6
              << " compute_ms=" << stats.compute_ns / 1e6
              << " cdf_points=" << options.cdf_points << "\n";
  } catch (const std::exception& ex) {
    std::cerr << "Error: " << ex.what() << "\n";
    print_usage(argv[0]);
    return 1;
  }

  return 0;
}
