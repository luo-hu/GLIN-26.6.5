// Synthetic point data generator for GLIN benchmarks.
//
// This mirrors the point distributions used by learnedbench:
//   uniform:    each dimension independently samples U(0, scale)
//   gaussian:   each dimension independently samples N(0, scale^2)
//   lognormal:  each dimension independently samples lognormal(mean=0, sigma=scale)
//
// Output formats:
//   binary: raw double stream, point coordinates are written dimension by dimension.
//           This matches learnedbench's TPIE file layout without depending on TPIE.
//   wkt:    one 2D POINT WKT per line, readable by bench_glin_wkt and baselines.
//   both:   write both binary and WKT files.

#include <fstream>
#include <iomanip>
#include <iostream>
#include <algorithm>
#include <cmath>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Options {
  std::string dist = "uniform";
  std::string output_file;
  std::string binary_file;
  std::string wkt_file;
  std::string format = "wkt";
  std::size_t num = 10000;
  int dim = 2;
  double scale = 1.0;
  bool map_geo = false;
  double lon_min = -180.0;
  double lon_max = 180.0;
  double lat_min = -90.0;
  double lat_max = 90.0;
  double diag_noise = 0.01;
  std::uint64_t seed = 42;
};

void print_usage(const char* program) {
  std::cerr
      << "Usage: " << program << " --output_file data/synthetic/uniform_1m_2_1.wkt [options]\n"
      << "Options:\n"
      << "  --dist uniform|unif|diag|gaussian|lognormal  Distribution type (default: uniform)\n"
      << "  --num N                            Number of points (default: 10000)\n"
      << "  --dim D                            Point dimension (default: 2)\n"
      << "  --scale S                          Distribution scale/range (default: 1)\n"
      << "  --seed N                           Random seed (default: 42)\n"
      << "  --format wkt|binary|both           Output format (default: wkt)\n"
      << "  --map_geo                          Map 2D unit-square data to lon/lat range\n"
      << "  --lon_min X                        Longitude minimum for --map_geo (default: -180)\n"
      << "  --lon_max X                        Longitude maximum for --map_geo (default: 180)\n"
      << "  --lat_min Y                        Latitude minimum for --map_geo (default: -90)\n"
      << "  --lat_max Y                        Latitude maximum for --map_geo (default: 90)\n"
      << "  --diag_noise S                     Diagonal noise in unit space (default: 0.01)\n"
      << "  --output_file PATH                 Output path for wkt or binary mode\n"
      << "  --wkt_file PATH                    WKT path for --format both\n"
      << "  --binary_file PATH                 Binary path for --format both\n";
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

    if (key == "--dist") {
      options.dist = require_value(key);
    } else if (key == "--num" || key == "-n") {
      options.num = static_cast<std::size_t>(std::stoull(require_value(key)));
    } else if (key == "--dim" || key == "-d") {
      options.dim = std::stoi(require_value(key));
    } else if (key == "--scale" || key == "-s") {
      options.scale = std::stod(require_value(key));
    } else if (key == "--map_geo") {
      options.map_geo = true;
    } else if (key == "--lon_min") {
      options.lon_min = std::stod(require_value(key));
    } else if (key == "--lon_max") {
      options.lon_max = std::stod(require_value(key));
    } else if (key == "--lat_min") {
      options.lat_min = std::stod(require_value(key));
    } else if (key == "--lat_max") {
      options.lat_max = std::stod(require_value(key));
    } else if (key == "--diag_noise") {
      options.diag_noise = std::stod(require_value(key));
    } else if (key == "--seed") {
      options.seed = static_cast<std::uint64_t>(std::stoull(require_value(key)));
    } else if (key == "--format") {
      options.format = require_value(key);
    } else if (key == "--output_file" || key == "-f") {
      options.output_file = require_value(key);
    } else if (key == "--binary_file") {
      options.binary_file = require_value(key);
    } else if (key == "--wkt_file") {
      options.wkt_file = require_value(key);
    } else if (key == "--help" || key == "-h") {
      print_usage(argv[0]);
      std::exit(0);
    } else {
      throw std::runtime_error("Unknown option: " + key);
    }
  }

  if (options.dist == "unif") {
    options.dist = "uniform";
  }
  if (options.dist != "uniform" && options.dist != "diag" &&
      options.dist != "gaussian" && options.dist != "lognormal") {
    throw std::runtime_error("--dist must be uniform, unif, diag, gaussian, or lognormal");
  }
  if (options.format != "wkt" && options.format != "binary" &&
      options.format != "both") {
    throw std::runtime_error("--format must be wkt, binary, or both");
  }
  if (options.num == 0) {
    throw std::runtime_error("--num must be greater than 0");
  }
  if (options.dim <= 0) {
    throw std::runtime_error("--dim must be greater than 0");
  }
  if ((options.format == "wkt" || options.format == "both") &&
      options.dim != 2) {
    throw std::runtime_error("WKT output requires --dim 2");
  }
  if (options.map_geo && options.dim != 2) {
    throw std::runtime_error("--map_geo requires --dim 2");
  }
  if (options.map_geo &&
      (options.dist == "gaussian" || options.dist == "lognormal")) {
    throw std::runtime_error("--map_geo is intended for uniform/unif/diag unit-square data");
  }
  if (options.lon_min >= options.lon_max || options.lat_min >= options.lat_max) {
    throw std::runtime_error("Invalid lon/lat range");
  }
  if ((options.format == "wkt" || options.format == "binary") &&
      options.output_file.empty()) {
    throw std::runtime_error("--output_file is required for wkt or binary mode");
  }
  if (options.format == "both" &&
      (options.wkt_file.empty() || options.binary_file.empty())) {
    throw std::runtime_error("--format both requires --wkt_file and --binary_file");
  }
  return options;
}

class Sampler {
 public:
  explicit Sampler(const Options& options)
      : options_(options),
        generator_(options.seed),
        uniform_(0.0, options.scale),
        unit_(0.0, 1.0),
        gaussian_(0.0, options.scale),
        lognormal_(0.0, options.scale),
        diagonal_noise_(0.0, options.diag_noise) {}

  double next() {
    if (options_.dist == "uniform") {
      return uniform_(generator_);
    }
    if (options_.dist == "gaussian") {
      return gaussian_(generator_);
    }
    return lognormal_(generator_);
  }

  std::vector<double> next_point() {
    std::vector<double> point(static_cast<std::size_t>(options_.dim));
    if (options_.dist == "diag") {
      if (options_.dim != 2) {
        throw std::runtime_error("diag distribution requires --dim 2");
      }
      const double t = unit_(generator_);
      point[0] = t;
      point[1] = std::max(0.0, std::min(1.0, t + diagonal_noise_(generator_)));
    } else {
      for (int j = 0; j < options_.dim; ++j) {
        point[static_cast<std::size_t>(j)] = next();
      }
    }

    if (options_.map_geo) {
      point[0] = options_.lon_min + point[0] * (options_.lon_max - options_.lon_min);
      point[1] = options_.lat_min + point[1] * (options_.lat_max - options_.lat_min);
    }
    return point;
  }

 private:
  const Options& options_;
  std::mt19937_64 generator_;
  std::uniform_real_distribution<double> uniform_;
  std::uniform_real_distribution<double> unit_;
  std::normal_distribution<double> gaussian_;
  std::lognormal_distribution<double> lognormal_;
  std::normal_distribution<double> diagonal_noise_;
};

void write_binary(const Options& options) {
  const std::string path =
      options.format == "both" ? options.binary_file : options.output_file;
  std::ofstream output(path, std::ios::binary);
  if (!output) {
    throw std::runtime_error("Cannot open binary output file: " + path);
  }

  Sampler sampler(options);
  for (std::size_t i = 0; i < options.num; ++i) {
    const std::vector<double> point = sampler.next_point();
    output.write(reinterpret_cast<const char*>(point.data()),
                 sizeof(double) * point.size());
  }
}

void write_wkt(const Options& options) {
  const std::string path =
      options.format == "both" ? options.wkt_file : options.output_file;
  std::ofstream output(path);
  if (!output) {
    throw std::runtime_error("Cannot open WKT output file: " + path);
  }

  Sampler sampler(options);
  output << std::setprecision(17);
  for (std::size_t i = 0; i < options.num; ++i) {
    const std::vector<double> point = sampler.next_point();
    output << "\"POINT (" << point[0] << " " << point[1] << ")\"\n";
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  try {
    Options options = parse_args(argc, argv);
    if (options.format == "binary" || options.format == "both") {
      write_binary(options);
    }
    if (options.format == "wkt" || options.format == "both") {
      write_wkt(options);
    }

    std::cout << "dist=" << options.dist
              << " num=" << options.num
              << " dim=" << options.dim
              << " scale=" << options.scale
              << " map_geo=" << (options.map_geo ? 1 : 0)
              << " seed=" << options.seed
              << " format=" << options.format << "\n";
  } catch (const std::exception& ex) {
    std::cerr << "Error: " << ex.what() << "\n";
    print_usage(argv[0]);
    return 1;
  }

  return 0;
}
