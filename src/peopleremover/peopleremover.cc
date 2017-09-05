#include <unordered_map>
#include <set>
#include <boost/functional/hash.hpp>
#include <boost/program_options.hpp>
#include <limits>
#include <iostream>
#include <fstream>
#include <sys/stat.h>
#include <sys/types.h>

namespace po = boost::program_options;

#include "scanio/scan_io.h"
#include "slam6d/scan.h"
#include "slam6d/normals.h"
#include "spherical_quadtree/spherical_quadtree.h"

#ifdef _OPENMP
#include <omp.h>
#endif

/*
 * instead of this struct we could also use an std::tuple which would be
 * equally fast and use the same amount of memory but I just don't like the
 * verbosity of the syntax and how to access members via std::get<>(v)
 */
struct voxel{
	mutable ssize_t x;
	mutable ssize_t y;
	mutable ssize_t z;

	voxel(const ssize_t X, const ssize_t Y, const ssize_t Z)
		: x(X), y(Y), z(Z) {}

	voxel(const struct voxel &other)
		: x(other.x), y(other.y), z(other.z) {}

	bool operator<(const struct voxel& rhs) const
	{
		if (x != rhs.x) {
			return x < rhs.x;
		}
		if (y != rhs.y) {
			return y < rhs.y;
		}
		return z < rhs.z;
	}

	bool operator==(const struct voxel& rhs) const
	{
		return x == rhs.x && y == rhs.y && z == rhs.z;
	}

	bool operator!=(const struct voxel& rhs) const
	{
		return x != rhs.x || y != rhs.y || z != rhs.z;
	}
};

/*
 * define a hash function for struct voxel
 */
namespace std
{
	template<> struct hash<struct voxel>
	{
		std::size_t operator()(struct voxel const& t) const
		{
			std::size_t seed = 0;
			boost::hash_combine(seed, t.x);
			boost::hash_combine(seed, t.y);
			boost::hash_combine(seed, t.z);
			return seed;
		}
	};
};

/*
 * Integer division and modulo in C/C++ suck. We were told that the % operator
 * is the modulo but it is actually not. It calculates the remainder and not
 * the modulo. Furthermore, the % operator is incapable of operating on
 * floating point values. Lastly, even integer division is screwed up because
 * it rounds into different directions depending on whether the result is
 * positive or negative...
 *
 * We thus implement the sane and mathematically more correct behaviour of
 * Python here.
 */
ssize_t py_div(double a, double b)
{
	ssize_t q = a/b;
	double r = fmod(a,b);
	if ((r != 0) && ((r < 0) != (b < 0))) {
		q -= 1;
	}
	return q;
}

double py_mod(double a, double b)
{
	double r = fmod(a, b);
	if (r!=0 && ((r<0) != (b<0))) {
		r += b;
	}
	return r;
}

struct voxel voxel_of_point(const double *p, double voxel_size)
{
	return voxel(
			py_div(p[0], voxel_size),
			py_div(p[1], voxel_size),
			py_div(p[2], voxel_size));
}


// FIXME: test the runtime if empty_voxels and voxel_occupied_by_slice are not
// pointers
struct visitor_args
{
	std::set<struct voxel> *empty_voxels;
	std::unordered_map<struct voxel, std::set<size_t>> *voxel_occupied_by_slice;
	size_t current_slice;
	size_t diff;
};

bool visitor(struct voxel voxel, void *data)
{
	struct visitor_args *args = (struct visitor_args *)data;
	std::unordered_map<struct voxel, std::set<size_t>>::const_iterator scanslices_it =
		args->voxel_occupied_by_slice->find(voxel);
	// if voxel has no points at all, continue searching without marking
	// the current voxel as free as it had no points to begin with
	if (scanslices_it == args->voxel_occupied_by_slice->end()) {
		return true;
	}
	std::set<size_t> scanslices = scanslices_it->second;
	/*
	 * The following implements a sliding window within which voxels
	 * that also contain points with a similar index as the current
	 * slice are not marked as free. Instead, the search aborts
	 * early.
	 * If no points around the current slice are found in the voxel,
	 * then the points in it only were seen from a very different scanner
	 * position and thus, these points are actually not there and
	 * the voxel must be marked as free.
	 *
	 * if the voxel has a point in it, only abort if the slice number
	 * is close to the current slice
	 */
	if (args->diff == 0) {
		// if the voxel contains the current slice, abort the search
		if (scanslices.find(args->current_slice) != scanslices.end()) {
			return false;
		}
	} else {
		size_t window_start = args->current_slice - args->diff;
		// subtracting diff from an unsigned value might underflow it,
		// this check resets the window start to zero in that case
		if (args->diff > args->current_slice) {
			window_start = 0;
		}
		// first element that is equivalent or goes after value
		std::set<size_t>::iterator lower_bound = scanslices.lower_bound(window_start);
		// if elements in the set are found around the neighborhood of the
		// current slice, abort the search
		if (lower_bound != scanslices.end() && *lower_bound <= args->current_slice + args->diff) {
			return false;
		}
	}
	args->empty_voxels->insert(voxel);
	return true;
}

/*
 * walk voxels as described in
 *   Fast Voxel Traversal Algorithm for Ray Tracing
 *   by John Amanatides, Andrew Woo
 *   Eurographics ’87
 *   http://www.cs.yorku.ca/~amana/research/grid.pdf
 */
void walk_voxels(
		const double * const start_pos,
		const double * const end_pos,
		const double voxel_size,
		bool (*visitor)(struct voxel, void *),
		void *data
		)
{
	double *direction = new double[3];
	direction[0] = end_pos[0] - start_pos[0];
	direction[1] = end_pos[1] - start_pos[1];
	direction[2] = end_pos[2] - start_pos[2];
	if (direction[0] == 0 && direction[1] == 0 && direction[2] == 0) {
		// FIXME: should we really abort here? Should we not at least call
		// visitor() on the start_voxel?
		return;
	}
	const struct voxel start_voxel = voxel_of_point(start_pos,voxel_size);
	struct voxel cur_voxel = voxel(start_voxel);
	const struct voxel end_voxel = voxel_of_point(end_pos,voxel_size);
	visitor(start_voxel, data);
	if (start_voxel.x == end_voxel.x && start_voxel.y == end_voxel.y && start_voxel.z == end_voxel.z) {
		return;
	}
	double tDeltaX, tMaxX, maxMultX;
	double tDeltaY, tMaxY, maxMultY;
	double tDeltaZ, tMaxZ, maxMultZ;
	char stepX, stepY, stepZ;
	/*
	 * tMax*: value t at which the segment crosses the first voxel boundary in the given direction
	 * stepX: in which direction to increase the voxel count (1 or -1)
	 * tDelta*: value t needed to span the voxel size in the given direction up to the target
	 */
	if (direction[0] == 0) {
		tDeltaX = 0;
		stepX = 0;
		tMaxX = std::numeric_limits<double>::infinity();
		maxMultX = std::numeric_limits<double>::infinity();
	} else {
		stepX = direction[0] > 0 ? 1 : -1;
		tDeltaX = stepX*voxel_size/direction[0];
		tMaxX = tDeltaX * (1.0 - py_mod(stepX*(start_pos[0]/voxel_size), 1.0));
		maxMultX = (end_voxel.x - start_voxel.x)*stepX;
		if (stepX == -1 && tMaxX == tDeltaX && start_voxel.x != end_voxel.x) {
			cur_voxel.x -= 1;
			start_voxel.x -= 1;
			maxMultX -= 1;
		}
	}
	if (direction[1] == 0) {
		tDeltaY = 0;
		stepY = 0;
		tMaxY = std::numeric_limits<double>::infinity();
		maxMultY = std::numeric_limits<double>::infinity();
	} else {
		stepY = direction[1] > 0 ? 1 : -1;
		tDeltaY = stepY*voxel_size/direction[1];
		tMaxY = tDeltaY * (1.0 - py_mod(stepY*(start_pos[1]/voxel_size), 1.0));
		maxMultY = (end_voxel.y - start_voxel.y)*stepY;
		if (stepY == -1 && tMaxY == tDeltaY && start_voxel.y != end_voxel.y) {
			cur_voxel.y -= 1;
			start_voxel.y -= 1;
			maxMultY -= 1;
		}
	}
	if (direction[2] == 0) {
		tDeltaZ = 0;
		stepZ = 0;
		tMaxZ = std::numeric_limits<double>::infinity();
		maxMultZ = std::numeric_limits<double>::infinity();
	} else {
		stepZ = direction[2] > 0 ? 1 : -1;
		tDeltaZ = stepZ*voxel_size/direction[2];
		tMaxZ = tDeltaZ * (1.0 - py_mod(stepZ*(start_pos[2]/voxel_size), 1.0));
		maxMultZ = (end_voxel.z - start_voxel.z)*stepZ;
		if (stepZ == -1 && tMaxZ == tDeltaZ && start_voxel.z != end_voxel.z) {
			cur_voxel.z -= 1;
			start_voxel.z -= 1;
			maxMultZ -= 1;
		}
	}
	// FIXME: don't call visitor() unconditionally but only if cur_voxel
	// unequal start_voxel
	visitor(cur_voxel, data);
	if (cur_voxel.x == end_voxel.x && cur_voxel.y == end_voxel.y && cur_voxel.z == end_voxel.z) {
		return;
	}
	/*
	 * in contrast to the original algorithm by John Amanatides and Andrew Woo
	 * we increment a counter and multiply the step size instead of adding up
	 * the steps. Doing the latter might introduce errors because due to
	 * floating point precision errors, 0.1+0.1+0.1 is unequal 3*0.1.
	 */
	size_t multX = 0, multY = 0, multZ = 0;
	double tMaxXStart = tMaxX;
	double tMaxYStart = tMaxY;
	double tMaxZStart = tMaxZ;
	/*
	 * iterate until either:
	 *  - the final voxel is reached
	 *  - tMax is reached by all tMax-coordinates
	 *  - the current voxel contains points of (or around) the current scan
	 */
	while (1) {
		double minVal = std::min(std::min(tMaxX, tMaxY), tMaxZ);
		bool steppedX = false, steppedY = false, steppedZ = false;
		if (minVal == tMaxX) {
			multX += 1;
			cur_voxel.x = start_voxel.x + multX*stepX;
			tMaxX = tMaxXStart + multX*tDeltaX;
			steppedX = true;
		}
		if (minVal == tMaxY) {
			multY += 1;
			cur_voxel.y = start_voxel.y + multY*stepY;
			tMaxY = tMaxYStart + multY*tDeltaY;
			steppedY = true;
		}
		if (minVal == tMaxZ) {
			multZ += 1;
			cur_voxel.z = start_voxel.z + multZ*stepZ;
			tMaxZ = tMaxZStart + multZ*tDeltaZ;
			steppedZ = true;
		}
		/*
		 * If we end up stepping in more than one direction at the same time,
		 * then we must also assess if we have to add the voxel that we just
		 * "graced" in the process.
		 * An additional voxel must only be added in six of the eight different
		 * directions that we can step into. If we step in all positive or
		 * in all negative directions, then no additional voxel is added.
		 */
		if (((steppedX && steppedY) || (steppedY && steppedZ) || (steppedX && steppedZ))
		&& (stepX == 1 || stepY == 1 || stepZ == 1) && (stepX == -1 || stepY == -1 || stepZ == -1)) {
			struct voxel add_voxel(cur_voxel);
			/*
			 * a voxel was only possibly missed if we stepped into a
			 * negative direction and if that step was actually carried
			 * out in this iteration
			 */
			if (steppedX) {
				if (stepX < 0) {
					if (multX > maxMultX + 1) {
						break;
					}
					add_voxel.x += 1;
				} else if (multX > maxMultX) {
					break;
				}
			}
			if (steppedY) {
				if (stepY < 0) {
					if (multY > maxMultY + 1) {
						break;
					}
					add_voxel.y += 1;
				} else if (multY > maxMultY) {
					break;
				}
			}
			if (steppedZ) {
				if (stepZ < 0) {
					if (multZ > maxMultZ + 1) {
						break;
					}
					add_voxel.z += 1;
				} else if (multZ > maxMultZ) {
					break;
				}
			}
			// FIXME: only call visitor if add_voxel unequal cur_voxel
			if (!visitor(add_voxel, data)) {
				break;
			}
		}
		/*
		 * non-exact versions of this algorithm might never reach the end voxel,
		 * so we abort early using a different criterion
		 */
		if (steppedX && multX > maxMultX) {
			break;
		}
		if (steppedY && multY > maxMultY) {
			break;
		}
		if (steppedZ && multZ > maxMultZ) {
			break;
		}
		if (!visitor(cur_voxel, data)) {
			break;
		}
	}
	delete[] direction;
	return;
}

void validate(boost::any& v, const std::vector<std::string>& values,
  IOType* target_type, int)
{
  using namespace boost::program_options;

  validators::check_first_occurrence(v);
  const std::string& s = validators::get_single_string(values);

  try {
    v = boost::any(formatname_to_io_type(s.c_str()));
  } catch (const std::runtime_error &e) {
    throw validation_error(validation_error::invalid_option_value, "Error due to --format (" + s + "): " + e.what());
  }
}

enum maxrange_method_t {NONE, NORMALS, ONENEAREST};

void validate(boost::any& v, const std::vector<std::string>& values,
		maxrange_method_t* target_type, int)
{
	using namespace boost::program_options;

	validators::check_first_occurrence(v);
	const std::string& s = validators::get_single_string(values);

	std::unordered_map<std::string, maxrange_method_t> method_values({
			{"none", NONE},
			{"normals", NORMALS},
			{"1nearest", ONENEAREST}
			});

	try {
		v = boost::any(method_values.at(s));
	} catch (std::out_of_range) {
		throw validation_error(validation_error::invalid_option_value, "Unknown maxrange method: " + s);
	}
}

enum normal_method_t {KNEAREST, RANGE, ANGLE, KNEAREST_GLOBAL, RANGE_GLOBAL};

void validate(boost::any& v, const std::vector<std::string>& values,
		normal_method_t* target_type, int)
{
	using namespace boost::program_options;

	validators::check_first_occurrence(v);
	const std::string& s = validators::get_single_string(values);

	std::unordered_map<std::string, normal_method_t> method_values({
			{"knearest", KNEAREST},
			{"range", RANGE},
			{"angle", ANGLE},
			{"knearest-global", KNEAREST_GLOBAL},
			{"range-global", RANGE_GLOBAL}
			});

	try {
		v = boost::any(method_values.at(s));
	} catch (std::out_of_range) {
		throw validation_error(validation_error::invalid_option_value, "Unknown normal method: " + s);
	}
}

int main(int argc, char* argv[])
{
	ssize_t start, end;
	IOType format;
	double fuzz;
	double voxel_size;
	size_t diff, normal_knearest;
	normal_method_t normal_method;
	maxrange_method_t maxrange_method;
	std::string maskdir;
	std::string dir;
	bool no_subvoxel_accuracy;
	int jobs;

	po::options_description general_options("General options");
	general_options.add_options()
		("help,h", "show this help message and exit")
		;

	po::options_description input_options("Input options");
	input_options.add_options()
		("start,s", po::value(&start)->default_value(0), "Start at this scan number (0-based)")
		("end,e", po::value(&end)->default_value(-1), "Stop at this scan number (0-based, with -1 meaning don't stop)")
		("format,f", po::value(&format)->default_value(UOS, "uos"), 
		 "The input files are read with this shared library.\n"
		 "Available values: uos, uos_map, uos_rgb, uos_frames, uos_map_frames, "
		 "old, rts, rts_map, ifp, riegl_txt, riegl_rgb, riegl_bin, zahn, ply, "
		 "wrl, xyz, zuf, iais, front, x3d, rxp, ais.")
		;

	po::options_description program_options("Program specific options");
	program_options.add_options()
		("fuzz", po::value(&fuzz)->default_value(0), 
		 "How fuzzy the data is. I.e. how far points on a"
		 "perfect plane are allowed to lie away from it in the"
		 "scan (default: 0).")
		("voxel-size", po::value(&voxel_size)->default_value(10), "Voxel grid size (default: 10)")
		("diff", po::value(&diff)->default_value(0), "Number of scans before and after the current scan that are grouped together (default: 0).")
		("no-subvoxel-accuracy", po::bool_switch(&no_subvoxel_accuracy)->default_value(false), "Do not calculate with subvoxel accuracy")
		("maxrange-method", po::value(&maxrange_method)->default_value(NONE),
		 "How to compute search range. Possible values: none, normals, 1nearest")
		("normal-knearest", po::value(&normal_knearest)->default_value(40),
		 "To compute the normal vector, use NUM closest points for --maxrange-method=normals (default: 40)")
		("normal-method", po::value(&normal_method)->default_value(ANGLE),
		 "How to select points to compute the normal from. "
		 "Possible values: knearest (choose k using --normal-"
		 "knearest), range (range search of voxel radius), angle"
		 "(all points seen under the angle that one voxel is"
		 "seen from the perspective of the scanner), knearest-"
		 "global (like knearest but from a global k-d tree),"
		 "rangle-global (like range but from a global k-d tree)."
		 "Default: angle")
		("maskdir", po::value(&maskdir), "Directory to store .mask files. Default: ${directory}/pplremover")
#ifdef _OPENMP
		("jobs,j", po::value<int>(&jobs)->default_value(1),
		 "number of threads to run in parallel. Default: 1")
#endif
		;

	po::options_description hidden_options("Hidden options");
	hidden_options.add_options()
		("input-dir", po::value<std::string>(&dir), "input dir");

	// all options
	po::options_description all_options;
	all_options.add(general_options).add(input_options).add(program_options).add(hidden_options);

	// options visible with --help
	po::options_description cmdline_options;
	cmdline_options.add(general_options).add(input_options).add(program_options);

	// positional argument
	po::positional_options_description pd;
	pd.add("input-dir", 1);

	// Parse the options into this map
	po::variables_map vm;
	po::store(
			po::command_line_parser(argc, argv)
			.options(all_options)
			.positional(pd)
			.run()
			, vm);

	// Help text
	if (vm.count("help")) {
		std::cout << "Usage2: " << argv[0] << " [options] <input-dir>" << std::endl;
		std::cout << cmdline_options << std::endl;
		exit(0);
	}

	po::notify(vm);

	// Scan number range
	if (start < 0) {
		throw std::logic_error("Cannot start at a negative scan number.");
	}
	if (end < -1) {
		throw std::logic_error("Cannot end at a negative scan number.");
	}
	if (0 < end && end < start) {
		throw std::logic_error("<end> (" + to_string(end) + ") cannot be smaller than <start> (" + to_string(start) + ").");
	}

	const char separator =
#ifdef _MSC_VER
		'\\';
#else
		'/';
#endif

	if (vm.count("input-dir")) {
		if (dir.back() != separator) {
			dir += separator;
		}
	}

	double voxel_diagonal = sqrt(3*voxel_size*voxel_size);

	std::cout << "dir: " << dir << std::endl;

	Scan::openDirectory(false, dir, format, start, end);
	if(Scan::allScans.size() == 0) {
		std::cerr << "No scans found. Did you use the correct format?" << std::endl;
		exit(-1);
	}

	std::unordered_map<size_t, DataXYZ> points_by_slice;
	std::unordered_map<size_t, DataReflectance> reflectances_by_slice;
	std::unordered_map<size_t, DataXYZ> orig_points_by_slice;
	std::unordered_map<size_t, std::tuple<const double *, const double *, const double *>> trajectory;
	std::cout << "size: " << Scan::allScans.size() << std::endl;
	std::vector<size_t> scanorder;
	for (size_t id = 0; id < Scan::allScans.size(); ++id) {
		size_t i = id+start;
		scanorder.push_back(i);
		Scan *scan = Scan::allScans[id];
		/* The range filter must be set *before* transformAll() because
		 * otherwise, transformAll will move the point coordinates such that
		 * the rangeFilter doesn't filter the right points anymore. This in
		 * turn can lead to the situation that when retrieving the reflectance
		 * values, some reflectance values are not returned because when
		 * reading them in, those get filtered by the *original* point
		 * coordinates. This in turn can lead to the situation that the vector
		 * for xyz and reflectance are of different length.
		 */
		scan->setRangeFilter(-1, voxel_diagonal);
		DataXYZ xyz_orig(scan->get("xyz"));
		// copy points
		size_t raw_orig_data_size = xyz_orig.size() * sizeof(double) * 3;
		unsigned char* xyz_orig_data = new unsigned char[raw_orig_data_size];
		memcpy(xyz_orig_data, xyz_orig.get_raw_pointer(), raw_orig_data_size);
		orig_points_by_slice[i] = DataPointer(xyz_orig_data, raw_orig_data_size);
		// now that the original coordinates are saved, transform
		scan->transformAll(scan->get_transMatOrg());
		trajectory[i] = std::make_tuple(scan->get_rPos(), scan->get_rPosTheta(), scan->get_transMatOrg());
		DataXYZ xyz(scan->get("xyz"));
		DataReflectance refl(scan->get("reflectance"));
		if (refl.size() != 0) {
			if (xyz.size() != refl.size() || xyz_orig.size() != refl.size()) {
				std::cerr << "refl mismatch: " << xyz.size() << " vs. " << refl.size() << std::endl;
				exit(1);
			}
			reflectances_by_slice[i] = refl;
		}
		points_by_slice[i] = xyz;
	}

	std::unordered_map<struct voxel, std::set<size_t>> voxel_occupied_by_slice;
	for (std::pair<size_t, DataXYZ> element : points_by_slice) {
		for (size_t i = 0; i < element.second.size(); ++i) {
			voxel_occupied_by_slice[voxel_of_point(element.second[i],voxel_size)].insert(element.first);
		}
	}

	if (voxel_occupied_by_slice.size() == 0) {
		std::cerr << "no voxel occupied" << std::endl;
		exit(1);
	}

	std::cerr << "occupied voxels: " << voxel_occupied_by_slice.size() << std::endl;

	std::cerr << "compute maxranges" << std::endl;
	std::unordered_map<size_t, std::vector<double>> maxranges;
	// FIXME: this is just wasting memory
	for (std::pair<size_t, std::tuple<const double*, const double*, const double*>> pose : trajectory) {
		size_t i = pose.first;
		std::vector<double> mr(points_by_slice[i].size(), std::numeric_limits<double>::infinity());
		maxranges[i] = mr;
	}

	if (maxrange_method == NORMALS) {
		if (normal_method == KNEAREST_GLOBAL || normal_method == RANGE_GLOBAL) {
			exit(1);
		}
#ifdef _OPENMP
	omp_set_num_threads(jobs);
#pragma omp parallel for schedule(dynamic)
#endif
		for (size_t idx = 0; idx < scanorder.size(); ++idx) {
			size_t i = scanorder[idx];
			/*
			const double* pos = std::get<0>(pose.second);
			const double* theta = std::get<1>(pose.second);
			const double* transmat = std::get<2>(pose.second);
			double transmat4inv[16];
			M4inv(transmat, transmat4inv);
			*/
			std::cerr << "building spherical quad tree" << std::endl;
			QuadTree qtree = QuadTree(orig_points_by_slice[i]);
			// no need to build a k-d tree for the "angle" method
			if (normal_method == KNEAREST || normal_method == RANGE) {
				exit(1);
			}
			std::cerr << "calculating ranges" << std::endl;
			// Precompute the distances so that they are not computed multiple
			// times while sorting
			std::vector<double> distances;
			std::vector<size_t> sorted_point_indices;
			for (size_t j = 0; j < orig_points_by_slice[i].size(); ++j) {
				distances.push_back(Len(orig_points_by_slice[i][j]));
				sorted_point_indices.push_back(j);
			}
			// sort points by their distance from the scanner but keep their
			// original index to update the correct corresponding entry in
			// maxranges
			std::stable_sort(sorted_point_indices.begin(), sorted_point_indices.end(),
					[&distances](const size_t a, const size_t b) -> bool {
						return distances[a] < distances[b];
					});
			// for each point in this scan (starting from the point closest to
			// the scanner) use its normal to calculate until when the line of
			// sight up to the point should be searched and apply the same limit
			// to all the points in its "shadow"
			for (size_t j : sorted_point_indices) {
				//double *p_global = points_by_slice[i][j];
				// FIXME: Len(p) is called multiple times - precompute or take
				// from vector of distances
				double *p = orig_points_by_slice[i][j];
				if (maxranges[i][j] != std::numeric_limits<double>::infinity()) {
					continue;
				}
				double p_norm[3] = {p[0], p[1], p[2]};
				Normalize3(p_norm);
				double normal[3];
				switch (normal_method) {
					case KNEAREST:
						exit(1);
						break;
					case RANGE:
						exit(1);
						break;
					case ANGLE:
						{
							double angle = 2*asin(voxel_diagonal/(distances[j]-voxel_diagonal));
							std::vector<size_t> angular_indices = qtree.search(p_norm, angle);
							std::vector<Point> angular_points;
							for (size_t k = 0; k < angular_indices.size(); ++k) {
								angular_points.push_back(Point(orig_points_by_slice[i][angular_indices[k]]));
							}
							double eigen[3];
							calculateNormal(angular_points, normal, eigen);
							break;
						}
					case KNEAREST_GLOBAL:
						exit(1);
						break;
					case RANGE_GLOBAL:
						exit(1);
						break;
					default:
						exit(1);
						break;
				}
				/*
				 * make sure that the normal vector points *toward* the scanner
				 * we don't need to compute the acos to get the real angle
				 * between the point vector and the normal because values less
				 * than zero map to angles of more than 90 degrees
				 */
				double angle_cos = normal[0]*p_norm[0]+normal[1]*p_norm[1]+normal[2]*p_norm[2];
				if (angle_cos >= 0) {
					// make sure that the normal turns toward the scanner
					normal[0] *= -1;
					normal[1] *= -1;
					normal[2] *= -1;
				}
				/*
				 * we only want to traverse the lines of sight until they hit
				 * the plane that lies a voxel diagonal above the current point
				 * in normal direction
				 * the base of that plane
				 */
				double p_base[3] = {
					p[0]+normal[0]*(voxel_diagonal+fuzz),
					p[1]+normal[1]*(voxel_diagonal+fuzz),
					p[2]+normal[2]*(voxel_diagonal+fuzz)};
				/*
				 * the dividend should stay the same as it's only dependent on
				 * the base of the plane
				 */
				double dividend = p_base[0]*normal[0]+p_base[1]*normal[1]+p_base[2]*normal[2];
				// calculate the divisor for the current point
				double divisor = p_norm[0]*normal[0]+p_norm[1]*normal[1]+p_norm[2]*normal[2];
				// normal vector is perpendicular to the line of sight up to
				// the point
				if (divisor == 0) {
					maxranges[i][j] = 0;
					continue;
				}
				if (dividend/divisor > distances[j]) {
					exit(1);
				}
				maxranges[i][j] = dividend/divisor;
				// the scanner itself is situated close to the plane that p is
				// part of. Thus, shoot no ray to this point at all.
				if (maxranges[i][j] < 0) {
					maxranges[i][j] = 0;
				}
				// points must not be too close or otherwise they will shadow
				// *all* the points
				// FIXME: retrieve Len(p) from vector of distances
				if (Len(p) < voxel_diagonal) {
					exit(1);
				}
				/*
				 * now find all the points in the shadow of this one
				 * the size of the shadow is determined by the angle under
				 * which the voxel diagonal is seen at that distance
				 *
				 * We compute the angle under which the circumsphere of a vox
				 * is seen to compute the shadow. We consider the worst case
				 * where the target point lies on the furthest point of the
				 * circumsphere. Thus, the center of the circumsphere is at
				 * the distance of the point minus its the radius of the
				 * circumsphere.
				 */
				// FIXME: if normal_method was "angle" we could just re-use
				// the points we already retrieved for normal computation
				// earlier here as well
				double angle = 2*asin(voxel_diagonal/(distances[j]-voxel_diagonal));
				for (size_t k : qtree.search(p_norm, angle)) {
					// FIXME: skip if j == k
					double *p_k = orig_points_by_slice[i][k];
					double p_k_norm[3] = {p_k[0], p_k[1], p_k[2]};
					Normalize3(p_k_norm);
					double divisor = p_k_norm[0]*normal[0]+p_k_norm[1]*normal[1]+p_k_norm[2]*normal[2];
					// normal vector is perpendicular to the line of sight up
					// to the point
					if (divisor == 0) {
						continue;
					}
					double d = dividend/divisor;
					/*
					 * even though p_k is further away from the scanner than p
					 * and inside the shadow of p, it is still on top of or
					 * in front of (from the point of view of the scanner) of
					 * the plane that p is part of. Thus, we process this point
					 * later
					 */
					if (d > distances[k]) {
						continue;
					}
					// the scanner itself is situated close to the plane that p
					// is part of. Thus, shoot no ray to this point at all.
					if (d < 0) {
						d = 0;
					}
					// point is already covered by a closer one
					if (maxranges[i][k] < d) {
						continue;
					}
					maxranges[i][k] = d;
				}
			}
		}
	} else if (maxrange_method == ONENEAREST) {
		exit(1);
	}
	// FIXME: fuzz also needs to be applied with maxrange_method == NONE

	std::set<struct voxel> free_voxels;

	std::cerr << "walk voxels" << std::endl;
	struct timespec before, after;
	clock_gettime(CLOCK_MONOTONIC, &before);
#ifdef _OPENMP
	omp_set_num_threads(jobs);
#pragma omp parallel for schedule(dynamic)
#endif
	for (size_t idx = 0; idx < scanorder.size(); ++idx) {
		size_t i = scanorder[idx];
		std::set<struct voxel> free;
		for (size_t j = 0; j < points_by_slice[i].size(); ++j) {
			double p[3] = {points_by_slice[i][j][0], points_by_slice[i][j][1], points_by_slice[i][j][2]};
			if (maxranges[i][j] != std::numeric_limits<double>::infinity()) {
				double maxrange = maxranges[i][j];
				double r = Len(orig_points_by_slice[i][j]);
				double factor = maxrange/r;
				p[0] = orig_points_by_slice[i][j][0]*factor;
				p[1] = orig_points_by_slice[i][j][1]*factor;
				p[2] = orig_points_by_slice[i][j][2]*factor;
				transform3(std::get<2>(trajectory[i]), p);
			}
			struct visitor_args data = {};
			data.empty_voxels = &free;
			data.voxel_occupied_by_slice = &voxel_occupied_by_slice;
			data.current_slice = i;
			data.diff = diff;
			walk_voxels(
					std::get<0>(trajectory[i]),
					p,
					voxel_size,
					visitor,
					&data
					);
		}
#ifdef _OPENMP
#pragma omp critical
#endif
		for (std::set<struct voxel>::iterator it = free.begin(); it != free.end(); ++it) {
			free_voxels.insert(*it);
		}
	}
	clock_gettime(CLOCK_MONOTONIC, &after);
	double elapsed = (after.tv_sec - before.tv_sec);
	elapsed += (after.tv_nsec - before.tv_nsec) / 1000000000.0;
	std::cerr << "took: " << elapsed << " seconds" << std::endl;

	std::cerr << "number of freed voxels: " << free_voxels.size() << " (" << (100*free_voxels.size()/voxel_occupied_by_slice.size()) << "% of occupied voxels)" << std::endl;

	if (!no_subvoxel_accuracy) {
		std::cerr << "calculate half-free voxels" << std::endl;
		exit(1);
	}

	std::cerr << "write partitioning" << std::endl;

	/*
	 * we use a FILE object instead of an ofstream to write the data because
	 * it gives better performance (probably because it's buffered) and
	 * because of the availability of fprintf
	 */
	FILE *out_static = fopen("scan000.3d", "w");
	FILE *out_dynamic = fopen("scan001.3d", "w");
	for (size_t i : scanorder) {
		for (size_t j = 0; j < points_by_slice[i].size(); ++j) {
			FILE *out;
			if (free_voxels.find(voxel_of_point(points_by_slice[i][j],voxel_size)) == free_voxels.end()) {
				out = out_static;
			} else {
				out = out_dynamic;
			}
			// we print the mantissa with 13 hexadecimal digits because the
			// mantissa for double precision is 52 bits long which is 6.5
			// bytes and thus 13 hexadecimal digits
			//
			// According to
			// http://pubs.opengroup.org/onlinepubs/000095399/functions/printf.html
			// the %a conversion specifier automatically picks the right
			// precision to represent the value exactly.
			fprintf(out, "%a %a %a %a\n",
					points_by_slice[i][j][0],
					points_by_slice[i][j][1],
					points_by_slice[i][j][2],
					0.0f
					//reflectances_by_slice[i][j]
					);
		}
	}
	fclose(out_static);
	fclose(out_dynamic);

	std::cerr << "write masks" << std::endl;

	if (maskdir == "") {
		maskdir = dir + "/pplremover";
	}
	// just mkdir it, ignore errors like EEXIST
	mkdir(maskdir.c_str(), S_IRWXU|S_IRWXG|S_IRWXO);
	for (std::pair<size_t, DataXYZ> element : points_by_slice) {
		std::ostringstream out;
		out << maskdir << "/scan" << std::setw(3) << std::setfill('0') << element.first << ".mask";
		FILE *out_mask = fopen(out.str().c_str(), "w");
		for (size_t i = 0; i < element.second.size(); ++i) {
			if (free_voxels.find(voxel_of_point(element.second[i],voxel_size)) == free_voxels.end()) {
				fprintf(out_mask, "0\n");
			} else {
				fprintf(out_mask, "1\n");
			}
		}
		fclose(out_mask);
	}

	std::cerr << "done" << std::endl;

	return 0;
}
