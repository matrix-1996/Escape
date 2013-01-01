/**
 * $Id$
 */

#pragma once

#include <esc/common.h>
#include <gui/image/image.h>
#include <gui/button.h>

namespace gui {
	class ImageButton : public Button {
	public:
		ImageButton(Image *img,bool border = true)
			: Button(""), _img(img), _border(border) {
		};
		ImageButton(Image *img,gpos_t x,gpos_t y,gsize_t width,gsize_t height,bool border = true)
			: Button("",x,y,width,height), _img(img), _border(border) {
		};

		inline Image *getImage() const {
			return _img;
		};

		virtual gsize_t getPrefWidth() const;
		virtual gsize_t getPrefHeight() const;
		virtual void paintBorder(Graphics &g);
		virtual void paintBackground(Graphics &g);

	private:
		Image *_img;
		bool _border;
	};
}
