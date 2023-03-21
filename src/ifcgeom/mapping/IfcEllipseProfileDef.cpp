/********************************************************************************
 *                                                                              *
 * This file is part of IfcOpenShell.                                           *
 *                                                                              *
 * IfcOpenShell is free software: you can redistribute it and/or modify         *
 * it under the terms of the Lesser GNU General Public License as published by  *
 * the Free Software Foundation, either version 3.0 of the License, or          *
 * (at your option) any later version.                                          *
 *                                                                              *
 * IfcOpenShell is distributed in the hope that it will be useful,              *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of               *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the                 *
 * Lesser GNU General Public License for more details.                          *
 *                                                                              *
 * You should have received a copy of the Lesser GNU General Public License     *
 * along with this program. If not, see <http://www.gnu.org/licenses/>.         *
 *                                                                              *
 ********************************************************************************/

#include "mapping.h"
#define mapping POSTFIX_SCHEMA(mapping)
using namespace ifcopenshell::geometry;

taxonomy::item* mapping::map_impl(const IfcSchema::IfcEllipseProfileDef* inst) {
	double rx = inst->SemiAxis1() * length_unit_;
	double ry = inst->SemiAxis2() * length_unit_;
	const double tol = conv_settings_.getValue(ConversionSettings::GV_PRECISION);
	if (rx < tol || ry < tol) {
		Logger::Message(Logger::LOG_ERROR, "Radius not greater than zero for:", inst);
		return nullptr;
	}

	const bool rotated = ry > rx;

	Eigen::Matrix4d m4;
	bool has_position = true;
#ifdef SCHEMA_IfcParameterizedProfileDef_Position_IS_OPTIONAL
	has_position = !!inst->Position();
#endif
	if (has_position) {
		taxonomy::matrix4 m = as<taxonomy::matrix4>(map(inst->Position()));
		m4 = m.ccomponents();
	}

	if (ry > rx) {
		auto m4_copy = m4;
		m4 <<
			-m4_copy.col(1),
			m4_copy.col(0),
			m4_copy.col(2),
			m4_copy.col(3);
		std::swap(rx, ry);
	}

	auto fc = new taxonomy::face;
	auto lp = new taxonomy::loop;
	auto ed = new taxonomy::edge;
	auto el = new taxonomy::ellipse;
	el->radius = rx;
	el->radius2 = ry;
	ed->basis = el;
	lp->children.push_back(ed);
	fc->children.push_back(lp);
	return fc;
}
