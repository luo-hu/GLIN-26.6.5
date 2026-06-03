// Synthetic rectangle/polygon generator for GLIN Intersects benchmarks.
//
// This is intended as a local replacement when the GLIN paper's DIAG/UNIF
// synthetic datasets are unavailable. It writes one WKT POLYGON per line.
//
// Distributions:
//   uniform:  rectangle centers are uniform in the configured coordinate range.
//   diagonal: rectangle centers follow the lower-left to upper-right diagonal,
//             with configurable Gaussian noise around the diagonal.

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>

namespace {

struct Options {
  std::string dist = "uniform";
  std::string output_file;
  std::size_t num = 10000;
  double xmin = -180.0;
  double ymin = -90.0;
  double xmax = 180.0;
  double ymax = 90.0;
  double min_width = 0.0001;
  double max_width = 0.001;
  double min_height = 0.0001;
  double max_height = 0.001;
  double diagonal_noise = 0.01;
  std::uint64_t seed = 42;
};

void print_usage(const char* program) {
  std::cerr
      << "Usage: " << program << " --dist uniform --num 1000000 --output_file data/synthetic/UNIF_S.wkt [options]\n"
      << "Options:\n"
      << "  --dist uniform|diagonal   Rectangle center distribution (default: uniform)\n"
      << "  --num N                   Number of rectangles (default: 10000)\n"
      << "  --output_file PATH        Output WKT file\n"
      << "  --xmin X                  Coordinate range min x (default: -180)\n"
      << "  --ymin Y                  Coordinate range min y (default: -90)\n"
      << "  --xmax X                  Coordinate range max x (default: 180)\n"
      << "  --ymax Y                  Coordinate range max y (default: 90)\n"
      << "  --min_width W             Minimum rectangle width (default: 0.0001)\n"
      << "  --max_width W             Maximum rectangle width (default: 0.001)\n"
      << "  --min_height H            Minimum rectangle height (default: 0.0001)\n"
      << "  --max_height H            Maximum rectangle height (default: 0.001)\n"
      << "  --diagonal_noise F        Diagonal noise as fraction of y range (default: 0.01)\n"
      << "  --seed N                  Random seed (default: 42)\n";
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
    } else if (key == "--output_file" || key == "-f") {
      options.output_file = require_value(key);
    } else if (key == "--xmin") {
      options.xmin = std::stod(require_value(key));
    } else if (key == "--ymin") {
      options.ymin = std::stod(require_value(key));
    } else if (key == "--xmax") {
      options.xmax = std::stod(require_value(key));
    } else if (key == "--ymax") {
      options.ymax = std::stod(require_value(key));
    } else if (key == "--min_width") {
      options.min_width = std::stod(require_value(key));
    } else if (key == "--max_width") {
      options.max_width = std::stod(require_value(key));
    } else if (key == "--min_height") {
      options.min_height = std::stod(require_value(key));
    } else if (key == "--max_height") {
      options.max_height = std::stod(require_value(key));
    } else if (key == "--diagonal_noise") {
      options.diagonal_noise = std::stod(require_value(key));
    } else if (key == "--seed") {
      options.seed = static_cast<std::uint64_t>(std::stoull(require_value(key)));
    } else if (key == "--help" || key == "-h") {
      print_usage(argv[0]);
      std::exit(0);
    } else {
      throw std::runtime_error("Unknown option: " + key);
    }
  }

  if (options.dist != "uniform" && options.dist != "diagonal") {
    throw std::runtime_error("--dist must be uniform or diagonal");
  }
  if (options.output_file.empty()) {
    throw std::runtime_error("--output_file is required");
  }
  if (options.num == 0) {
    throw std::runtime_error("--num must be greater than 0");
  }
  if (options.xmin >= options.xmax || options.ymin >= options.ymax) {
    throw std::runtime_error("Invalid coordinate range");
  }
  if (options.min_width <= 0.0 || options.max_width < options.min_width ||
      options.min_height <= 0.0 || options.max_height < options.min_height) {
    throw std::runtime_error("Invalid rectangle width/height range");
  }
  if (options.diagonal_noise < 0.0) {
    throw std::runtime_error("--diagonal_noise must be non-negative");
  }
  return options;
}

double clamp(double value, double lo, double hi) {
  return std::max(lo, std::min(value, hi));
}

void write_rectangle(std::ofstream& output, double xmin, double ymin,
                     double xmax, double ymax) {
  output << "\"POLYGON ((" << xmin << " " << ymin << ", "
         << xmin << " " << ymax << ", "
         << xmax << " " << ymax << ", "
         << xmax << " " << ymin << ", "
         << xmin << " " << ymin << "))\"\n";
}

}  // namespace

int main(int argc, char* argv[]) {
  try {
    Options options = parse_args(argc, argv);

    std::ofstream output(options.output_file);
    if (!output) {
      throw std::runtime_error("Cannot open output file: " + options.output_file);
    }

    std::mt19937_64 generator(options.seed);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    std::uniform_real_distribution<double> xdist(options.xmin, options.xmax);
    std::uniform_real_distribution<double> ydist(options.ymin, options.ymax);
    std::uniform_real_distribution<double> wdist(options.min_width, options.max_width);
    std::uniform_real_distribution<double> hdist(options.min_height, options.max_height);
    std::normal_distribution<double> noise(0.0, options.diagonal_noise);

    const double xrange = options.xmax - options.xmin;
    const double yrange = options.ymax - options.ymin;

    output << std::setprecision(17);
    for (std::size_t i = 0; i < options.num; ++i) {
      double cx = 0.0;
      double cy = 0.0;
      if (options.dist == "uniform") {
        cx = xdist(generator);
        cy = ydist(generator);
      } else {
        const double t = unit(generator);
        cx = options.xmin + t * xrange;
        cy = options.ymin + clamp(t + noise(generator), 0.0, 1.0) * yrange;
      }

      const double width = wdist(generator);
      const double height = hdist(generator);
      double rxmin = clamp(cx - width / 2.0, options.xmin, options.xmax);
      double rxmax = clamp(cx + width / 2.0, options.xmin, options.xmax);
      double rymin = clamp(cy - height / 2.0, options.ymin, options.ymax);
      double rymax = clamp(cy + height / 2.0, options.ymin, options.ymax);
      if (rxmin == rxmax) {
        rxmax = clamp(rxmin + 1e-12, options.xmin, options.xmax);
      }
      if (rymin == rymax) {
        rymax = clamp(rymin + 1e-12, options.ymin, options.ymax);
      }
      write_rectangle(output, rxmin, rymin, rxmax, rymax);
    }

    std::cout << "dist=" << options.dist
              << " num=" << options.num
              << " output=" << options.output_file
              << " range=[" << options.xmin << "," << options.ymin << ","
              << options.xmax << "," << options.ymax << "]"
              << " width=[" << options.min_width << "," << options.max_width << "]"
              << " height=[" << options.min_height << "," << options.max_height << "]"
              << " seed=" << options.seed << "\n";
  } catch (const std::exception& ex) {
    std::cerr << "Error: " << ex.what() << "\n";
    print_usage(argv[0]);
    return 1;
  }

  return 0;
}
