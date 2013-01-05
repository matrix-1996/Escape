/**
 * $Id$
 * Copyright (C) 2008 - 2011 Nils Asmussen
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#pragma once

#include <esc/common.h>
#include <gui/layout/layout.h>
#include <gui/control.h>

namespace gui {
	/**
	 * The flowlayout puts all controls side by side with their minimum widths and heights.
	 * The alignment of all controls can be left, center and right.
	 */
	class FlowLayout : public Layout {
	public:
		enum Align {
			FRONT,
			CENTER,
			BACK,
		};
		enum Orientation {
			HORIZONTAL,
			VERTICAL
		};

		static const gsize_t DEF_GAP	= 2;

		/**
		 * Constructor
		 *
		 * @param align the alignment of the controls
		 * @param orientation the orientation
		 * @param gap the gap between the controls (default 2)
		 */
		FlowLayout(Align align,Orientation orientation = HORIZONTAL,gsize_t gap = DEF_GAP)
			: Layout(), _align(align), _orientation(orientation), _gap(gap), _p(), _ctrls() {
		}

		virtual void add(Panel *p,std::shared_ptr<Control> c,pos_type pos);
		virtual void remove(Panel *p,std::shared_ptr<Control> c,pos_type pos);
		virtual void removeAll();

		virtual void rearrange();

	protected:
		virtual Size getSizeWith(const Size &avail,size_func func) const;

	private:
		Size getMaxSize() const;

		Align _align;
		Orientation _orientation;
		gsize_t _gap;
		Panel *_p;
		std::vector<std::shared_ptr<Control>> _ctrls;
	};
}
