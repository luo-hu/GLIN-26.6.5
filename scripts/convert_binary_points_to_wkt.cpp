// Convert learnedbench-style raw binary point files to WKT POINT rows.
//
// Input layout: no header, raw double stream. Every point stores dim doubles
// consecutively. By default the first two dimensions are written as x/y.

#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Options {
  std::string input_file;
  std::string output_file;
  std::size_t num = 0;
  int dim = 2;
  int x_col = 0;
  int y_col = 1;
};

void print_usage(const char* program) {
  std::cerr
      << "Usage: " << program
      << " --input_file data.bin --output_file data.wkt --num N --dim 2 [options]\n"
      << "Options:\n"
      << "  --x_col I   Coordinate column used as x (default: 0)\n"
      << "  --y_col I   Coordinate column used as y (default: 1)\n";
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

    if (key == "--input_file") {
      options.input_file = require_value(key);
    } else if (key == "--output_file") {
      options.output_file = require_value(key);
    } else if (key == "--num" || key == "-n") {
      options.num = static_cast<std::size_t>(std::stoull(require_value(key)));
    } else if (key == "--dim" || key == "-d") {
      options.dim = std::stoi(require_value(key));
    } else if (key == "--x_col") {
      options.x_col = std::stoi(require_value(key));
    } else if (key == "--y_col") {
      options.y_col = std::stoi(require_value(key));
    } else if (key == "--help" || key == "-h") {
      print_usage(argv[0]);
      std::exit(0);
    } else {
      throw std::runtime_error("Unknown option: " + key);
    }
  }

  if (options.input_file.empty()) {
    throw std::runtime_error("--input_file is required");
  }
  if (options.output_file.empty()) {
    throw std::runtime_error("--output_file is required");
  }
  if (options.num == 0) {
    throw std::runtime_error("--num must be greater than 0");
  }
  if (options.dim <= 0) {
    throw std::runtime_error("--dim must be greater than 0");
  }
  if (options.x_col < 0 || options.x_col >= options.dim ||
      options.y_col < 0 || options.y_col >= options.dim) {
    throw std::runtime_error("--x_col and --y_col must be inside [0, dim)");
  }
  if (options.x_col == options.y_col) {
    throw std::runtime_error("--x_col and --y_col must be different");
  }
  return options;
}

}  // namespace

int main(int argc, char* argv[]) {
  try {
    Options options = parse_args(argc, argv);

    std::ifstream input(options.input_file, std::ios::binary);
    if (!input) {
      throw std::runtime_error("Cannot open input file: " + options.input_file);
    }
    std::ofstream output(options.output_file);
    if (!output) {
      throw std::runtime_error("Cannot open output file: " + options.output_file);
    }

    std::vector<double> point(static_cast<std::size_t>(options.dim));
    output << std::setprecision(17);
    for (std::size_t i = 0; i < options.num; ++i) {
      input.read(reinterpret_cast<char*>(point.data()),
                 sizeof(double) * point.size());
      if (!input) {
        throw std::runtime_error("Input ended before reading requested points");
      }
      output << "\"POINT (" << point[static_cast<std::size_t>(options.x_col)]
             << " " << point[static_cast<std::size_t>(options.y_col)] << ")\"\n";
    }

    std::cout << "input=" << options.input_file
              << " output=" << options.output_file
              << " num=" << options.num
              << " dim=" << options.dim << "\n";
  } catch (const std::exception& ex) {
    std::cerr << "Error: " << ex.what() << "\n";
    print_usage(argv[0]);
    return 1;
  }

  return 0;
}
