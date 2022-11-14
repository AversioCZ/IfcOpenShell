#include "OpenCascadeKernel.h"

#include "IfcGeomTree.h"
#include "wire_utils.h"

using namespace ifcopenshell::geometry;
using namespace ifcopenshell::geometry::kernels;

namespace {
	void find_neighbours(ifcopenshell::geometry::impl::tree<int>& tree, std::vector<std::unique_ptr<gp_Pnt>>& pnts, std::set<int>& visited, int p, double eps) {
		visited.insert(p);

		Bnd_Box b;
		b.Set(*pnts[p].get());
		b.Enlarge(eps);

		std::vector<int> js = tree.select_box(b, false);
		for (int j : js) {
			visited.insert(j);
#ifdef FACESET_HELPER_RECURSIVE
			if (visited.find(j) == visited.end()) {
				// @todo, making this recursive removes the dependence on the initial ordering, but will
				// likely result in empty results when all vertices are within 1 eps from another point.
				find_neighbours(tree, pnts, visited, j, eps);
			}
#endif
		}
	}
}

OpenCascadeKernel::faceset_helper::faceset_helper(OpenCascadeKernel* kernel, const taxonomy::shell* shell)
	: kernel_(kernel)
	, non_manifold_(false)
{
	// @todo use pointers?
	std::vector<taxonomy::point3> points;
	std::vector<taxonomy::loop*> loops;

	for (auto& f : shell->children_as<taxonomy::face>()) {
		for (auto& l : f->children_as<taxonomy::loop>()) {
			loops.push_back(l);
			for (auto& e : l->children_as<taxonomy::edge>()) {
				// @todo make sure only cartesian points are provided here
				points.push_back(boost::get<taxonomy::point3>(e->start));
			}
		}
	}

	std::vector<std::unique_ptr<gp_Pnt>> pnts(points.size());
	std::vector<TopoDS_Vertex> vertices(pnts.size());

	// @todo
	impl::tree<int> tree;

	BRep_Builder B;

	Bnd_Box box;
	for (size_t i = 0; i < points.size(); ++i) {
		gp_Pnt* p = new gp_Pnt(convert_xyz<gp_Pnt>(points[i]));
		pnts[i].reset(p);
		B.MakeVertex(vertices[i], *p, Precision::Confusion());
		tree.add(i, vertices[i]);
		box.Add(*p);
	}

	// Use the bbox diagonal to influence local epsilon
	// double bdiff = std::sqrt(box.SquareExtent());

	// @todo the bounding box diagonal is not used (see above)
	// because we're explicitly interested in the miminal
	// dimension of the element to limit the tolerance (for sheet-
	// like elements for example). But the way below is very
	// dependent on orientation due to the usage of the
	// axis-aligned bounding box. Use PCA to find three non-aligned
	// set of dimensions and use the one with the smallest eigenvalue.

	// Find the minimal bounding box edge
	double bmin[3], bmax[3];
	box.Get(bmin[0], bmin[1], bmin[2], bmax[0], bmax[1], bmax[2]);
	double bdiff = std::numeric_limits<double>::infinity();
	for (size_t i = 0; i < 3; ++i) {
		const double d = bmax[i] - bmin[i];
		if (d > kernel->settings_.getValue(ConversionSettings::GV_PRECISION) * 10. && d < bdiff) {
			bdiff = d;
		}
	}

	eps_ = kernel->settings_.getValue(ConversionSettings::GV_PRECISION) * 10. * (std::min)(1.0, bdiff);

	// @todo, there a tiny possibility that the duplicate faces are triggered
	// for an internal boundary, that is also present as an external boundary.
	// This will result in non-manifold configuration then, but this is deemed
	// such as corner-case that it is not considered.

	size_t loops_removed, non_manifold, duplicate_faces;

	std::map<std::pair<int, int>, int> edge_use;

	for (int i = 0; i < 3; ++i) {
		// Some times files, have large tolerance values specified collapsing too many vertices.
		// This case we detect below and re-run the loop with smaller epsilon. Normally
		// the body of this loop would only be executed once.

		loops_removed = 0;
		non_manifold = 0;
		duplicate_faces = 0;

		vertex_mapping_.clear();
		duplicates_.clear();

		edge_use.clear();

		if (eps_ < Precision::Confusion()) {
			// occt uses some hard coded precision values, don't go smaller than that.
			// @todo, can be reset though with BRepLib::Precision(double)
			eps_ = Precision::Confusion();
		}

		for (int pnt_i = 0; pnt_i < (int)pnts.size(); ++pnt_i) {
			if (pnts[pnt_i]) {
				std::set<int> vs;
				find_neighbours(tree, pnts, vs, pnt_i, eps_);

				for (int v : vs) {
					auto& pt = points[v];
					// NB: insert() ignores duplicate keys
					vertex_mapping_.insert({ pt.instance->data().id() , i });
				}
			}
		}

		std::set<std::tuple<double, double, double>> unique;
		for (int pnt_i = 0; pnt_i < (int)pnts.size(); ++pnt_i) {
			if (pnts[pnt_i]) {
				unique.insert(std::make_tuple(
					(*pnts[pnt_i]).X(),
					(*pnts[pnt_i]).Y(),
					(*pnts[pnt_i]).Z()
				));
			}
		}

		if (unique.size() != vertex_mapping_.size()) {
			Logger::Notice("Collapsed vertices from " + std::to_string(pnts.size()) + " (" + std::to_string(unique.size()) + " unique) to " + std::to_string(vertex_mapping_.size()));
		}

		typedef std::array<int, 2> edge_t;
		typedef std::set<edge_t> edge_set_t;
		std::set<edge_set_t> edge_sets;

		for (auto& loop : loops) {
			std::vector<std::pair<int, int> > segments;
			edge_set_t segment_set;

			loop_(loop, [&segments, &segment_set](int C, int D, bool) {
				segment_set.insert(edge_t{ C,D });
				segments.push_back(std::make_pair(C, D));
			});

			if (edge_sets.find(segment_set) != edge_sets.end()) {
				duplicate_faces++;
				// @todo does this work with tesselated face sets, will they have an associated instance? Guess not.
				duplicates_.insert(loop->instance->data().id());
				continue;
			}
			edge_sets.insert(segment_set);

			if (segments.size() >= 3) {
				for (auto& p : segments) {
					edge_use[p] ++;
				}
			} else {
				loops_removed += 1;
			}
		}

		if (edge_use.size() != 0) {
			break;
		} else {
			eps_ /= 10.;
		}
	}

	for (auto& p : edge_use) {
		int a, b;
		std::tie(a, b) = p.first;
		edges_[p.first] = BRepBuilderAPI_MakeEdge(vertices[a], vertices[b]);

		if (p.second != 2) {
			non_manifold += 1;
		}
	}

	if (duplicates_.size() || loops_removed || (non_manifold && shell->closed.get_value_or(false))) {
		Logger::Warning(boost::lexical_cast<std::string>(duplicate_faces) + " duplicate faces removed, " + boost::lexical_cast<std::string>(loops_removed) + " degenerate loops eliminated and " + boost::lexical_cast<std::string>(non_manifold) + " non-manifold edges");
	}
}

void OpenCascadeKernel::faceset_helper::loop_(const taxonomy::loop* ps, const std::function<void(int, int, bool)>& callback) {
	if (ps->children.size() < 3) {
		return;
	}

	auto a = boost::get<taxonomy::point3>(((taxonomy::edge*) ps->children.back())->start).instance;
	auto A = a->data().id();
	for (auto& b : ps->children) {
		auto B = boost::get<taxonomy::point3>(((taxonomy::edge*) b)->start).instance->data().id();
		auto C = vertex_mapping_[A], D = vertex_mapping_[B];
		bool fwd = C < D;
		if (!fwd) {
			std::swap(C, D);
		}
		if (C != D) {
			callback(C, D, fwd);
			A = B;
		}
	}
}

bool OpenCascadeKernel::faceset_helper::edge(int A, int B, TopoDS_Edge& e) {
	auto it = edges_.find({ A, B });
	if (it == edges_.end()) {
		return false;
	}
	e = it->second;
	return true;
}

bool OpenCascadeKernel::faceset_helper::wire(const taxonomy::loop* loop, TopoDS_Wire& w) {
	TopTools_ListOfShape ws;
	if (!wires(loop, ws)) {
		return false;
	}
	util::select_largest(ws, w);
	return true;
}

bool OpenCascadeKernel::faceset_helper::wires(const taxonomy::loop* loop, TopTools_ListOfShape& wires) {
	if (duplicates_.find(loop->instance->data().id()) != duplicates_.end()) {
		return false;
	}
	TopoDS_Wire wire;
	BRep_Builder builder;
	builder.MakeWire(wire);
	int count = 0;
	loop_(loop, [this, &builder, &wire, &count](int A, int B, bool fwd) {
		TopoDS_Edge e;
		if (edge(A, B, e)) {
			if (!fwd) {
				e.Reverse();
			}
			builder.Add(wire, e);
			count += 1;
		}
	});
	if (count >= 3) {
		wire.Closed(true);

		TopTools_ListOfShape results;
		/* todo kernel_->getValue(GV_NO_WIRE_INTERSECTION_CHECK) < 0. &&  */
		/* todo kernel_->get_wire_intersection_tolerance(wire) */
		if (util::wire_intersections(wire, results, kernel_->settings_.getValue(ConversionSettings::GV_PRECISION), kernel_->settings_.getValue(ConversionSettings::GV_PRECISION))) {
			Logger::Warning("Self-intersections with " + boost::lexical_cast<std::string>(results.Extent()) + " cycles detected");
			non_manifold_ = true;
			wires = results;
		} else {
			wires.Append(wire);
		}

		return true;
	} else {
		return false;
	}
}

OpenCascadeKernel::faceset_helper::~faceset_helper() {
	// @todo this is super ugly, but how else can we be notified that the unique_ptr goes out of scope?
	// Perhaps just supply a custom std::deleter?
	kernel_->faceset_helper_ = nullptr;
}
