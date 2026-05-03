#pragma once

/**
 * glint_graphics.hpp
 * Standalone-owned graphics primitives used by glint.
 *
 * This header provides the small geometry, color, text, popup, and
 * Skia-backed image/SVG wrappers that the scene graph still expects.
 */

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

#include "include/core/SkColor.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkData.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMetrics.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkImage.h"
#include "include/core/SkPaint.h"
#include "include/core/SkBlendMode.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathEffect.h"
#include "include/effects/SkDashPathEffect.h"
#include "include/core/SkRect.h"
#include "include/core/SkStream.h"
#include "include/core/SkTextBlob.h"
#include "include/core/SkTypeface.h"
#include "modules/svg/include/SkSVGDOM.h"

#if defined(SK_BUILD_FOR_WIN)
#include "include/ports/SkTypeface_win.h"
#elif defined(SK_BUILD_FOR_MAC) || defined(SK_BUILD_FOR_IOS)
#include "include/ports/SkFontMgr_mac_ct.h"
#endif

namespace glint_graphics {

enum EAlign { Near = 0, Center, Far };
enum EVAlign { Top = 0, Middle, Bottom };
enum class EFillRule { Winding = 0, Preserve };

struct glint_halign
{
	EAlign value = EAlign::Near;

	glint_halign() = default;
	glint_halign(EAlign align) : value(align) {}
	glint_halign(const char* align) { assign(align); }
	glint_halign(const std::string& align) { assign(align); }

	glint_halign& operator=(EAlign align)
	{
		value = align;
		return *this;
	}

	glint_halign& operator=(const char* align)
	{
		assign(align);
		return *this;
	}

	glint_halign& operator=(const std::string& align)
	{
		assign(align);
		return *this;
	}

	operator EAlign() const { return value; }

	bool operator==(EAlign align) const { return value == align; }
	bool operator!=(EAlign align) const { return value != align; }

	friend bool operator==(EAlign lhs, const glint_halign& rhs) { return lhs == rhs.value; }
	friend bool operator!=(EAlign lhs, const glint_halign& rhs) { return lhs != rhs.value; }

private:
	void assign(const char* align)
	{
		if (!align) return;
		assign(std::string(align));
	}

	void assign(const std::string& align)
	{
		std::string low;
		low.reserve(align.size());
		for (char c : align)
			low.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

		if (low == "left" || low == "start" || low == "near") value = EAlign::Near;
		else if (low == "center" || low == "middle") value = EAlign::Center;
		else if (low == "right" || low == "end" || low == "far") value = EAlign::Far;
	}
};

struct glint_valign
{
	EVAlign value = EVAlign::Middle;

	glint_valign() = default;
	glint_valign(EVAlign align) : value(align) {}
	glint_valign(const char* align) { assign(align); }
	glint_valign(const std::string& align) { assign(align); }

	glint_valign& operator=(EVAlign align)
	{
		value = align;
		return *this;
	}

	glint_valign& operator=(const char* align)
	{
		assign(align);
		return *this;
	}

	glint_valign& operator=(const std::string& align)
	{
		assign(align);
		return *this;
	}

	operator EVAlign() const { return value; }

	bool operator==(EVAlign align) const { return value == align; }
	bool operator!=(EVAlign align) const { return value != align; }

	friend bool operator==(EVAlign lhs, const glint_valign& rhs) { return lhs == rhs.value; }
	friend bool operator!=(EVAlign lhs, const glint_valign& rhs) { return lhs != rhs.value; }

private:
	void assign(const char* align)
	{
		if (!align) return;
		assign(std::string(align));
	}

	void assign(const std::string& align)
	{
		std::string low;
		low.reserve(align.size());
		for (char c : align)
			low.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

		if (low == "top") value = EVAlign::Top;
		else if (low == "bottom") value = EVAlign::Bottom;
		else if (low == "middle" || low == "center") value = EVAlign::Middle;
	}
};

struct glint_fill_options
{
	bool preserve = false;
	EFillRule fillRule = EFillRule::Winding;

	glint_fill_options() = default;
	glint_fill_options(bool preservePath, EFillRule rule)
		: preserve(preservePath), fillRule(rule) {}
};

struct glint_color
{
	int A = 255;
	int R = 0;
	int G = 0;
	int B = 0;

	glint_color() = default;
	glint_color(int a, int r, int g, int b) : A(a), R(r), G(g), B(b) {}
};

struct glint_rect
{
	float L = 0.f;
	float T = 0.f;
	float R = 0.f;
	float B = 0.f;

	glint_rect() = default;
	glint_rect(float l, float t, float r, float b) : L(l), T(t), R(r), B(b) {}

	float W() const { return R - L; }
	float H() const { return B - T; }
	float MW() const { return (L + R) * 0.5f; }
	float MH() const { return (T + B) * 0.5f; }
	bool Contains(float x, float y) const { return x >= L && x <= R && y >= T && y <= B; }
	glint_rect GetPadded(float padding) const { return glint_rect(L - padding, T - padding, R + padding, B + padding); }
};

struct glint_text
{
	float mSize = 12.f;
	glint_color mFGColor {};
	std::string mFont {};
	glint_halign mAlign = EAlign::Near;
	glint_valign mVAlign = EVAlign::Middle;

	glint_text() = default;
	glint_text(float size, glint_color color, const char* font = nullptr,
				EAlign align = EAlign::Near, EVAlign valign = EVAlign::Middle)
		: mSize(size), mFGColor(color), mFont(font ? font : ""), mAlign(align), mVAlign(valign) {}
	glint_text(float size, glint_color color, const char* font, const char* align, const char* valign = nullptr)
		: mSize(size), mFGColor(color), mFont(font ? font : ""), mAlign(align ? align : "left"), mVAlign(valign ? valign : "middle") {}
	glint_text(float size, glint_color color, const char* font, const std::string& align, const std::string& valign = "middle")
		: mSize(size), mFGColor(color), mFont(font ? font : ""), mAlign(align), mVAlign(valign) {}
};

struct glint_pattern
{
	glint_color mColor {};

	glint_pattern() = default;
	explicit glint_pattern(glint_color color) : mColor(color) {}
};

class glint_api_bitmap
{
public:
	glint_api_bitmap() = default;
	explicit glint_api_bitmap(sk_sp<SkImage> image) : mImage(std::move(image)) {}

	bool IsValid() const { return static_cast<bool>(mImage); }
	SkImage* GetBitmap() const { return mImage.get(); }
	sk_sp<SkImage> image() const { return mImage; }

private:
	sk_sp<SkImage> mImage;
};

class glint_bitmap
{
public:
	glint_bitmap() = default;
	explicit glint_bitmap(sk_sp<SkImage> image, int numFrames = 1)
		: mBitmap(std::make_shared<glint_api_bitmap>(std::move(image))), mNumFrames(numFrames)
	{
		if (mBitmap && mBitmap->GetBitmap())
		{
			mW = static_cast<float>(mBitmap->GetBitmap()->width());
			mH = static_cast<float>(mBitmap->GetBitmap()->height());
		}
	}

	bool IsValid() const { return mBitmap && mBitmap->IsValid(); }
	glint_api_bitmap* GetAPIBitmap() const { return mBitmap.get(); }
	float W() const { return mW; }
	float H() const { return mH; }
	float FW() const { return mNumFrames > 0 ? mW / static_cast<float>(mNumFrames) : mW; }
	float FH() const { return mH; }

private:
	std::shared_ptr<glint_api_bitmap> mBitmap;
	float mW = 0.f;
	float mH = 0.f;
	int mNumFrames = 1;
};

class glint_svg
{
public:
	glint_svg() = default;
	glint_svg(std::nullptr_t) {}
	explicit glint_svg(sk_sp<SkSVGDOM> dom) : mDom(std::move(dom)) {}

	bool IsValid() const { return static_cast<bool>(mDom); }
	float W() const { return mSize.width(); }
	float H() const { return mSize.height(); }
	sk_sp<SkSVGDOM> dom() const { return mDom; }
	void setSize(SkSize size) { mSize = size; }

private:
	sk_sp<SkSVGDOM> mDom;
	SkSize mSize = SkSize::Make(0.f, 0.f);
};

class glint_popup_menu
{
public:
	class Item
	{
	public:
		static constexpr int kNoFlags = 0;
		static constexpr int kDisabled = 1 << 0;
		static constexpr int kChecked = 1 << 1;

		Item() = default;
		Item(std::string text, bool separator, int flags)
			: mText(std::move(text)), mSeparator(separator), mFlags(flags) {}

		bool GetIsSeparator() const { return mSeparator; }
		bool GetEnabled() const { return (mFlags & kDisabled) == 0; }
		bool GetChecked() const { return (mFlags & kChecked) != 0; }
		const char* GetText() const { return mText.c_str(); }

	private:
		std::string mText;
		bool mSeparator = false;
		int mFlags = kNoFlags;
	};

	glint_popup_menu() = default;

	void AddItem(const char* text, int /*unused*/ = -1, int flags = Item::kNoFlags)
	{
		mItems.emplace_back(text ? text : "", false, flags);
	}

	void AddSeparator()
	{
		mItems.emplace_back("", true, Item::kNoFlags);
	}

	int NItems() const { return static_cast<int>(mItems.size()); }
	const Item* GetItem(int index) const { return index >= 0 && index < NItems() ? &mItems[static_cast<size_t>(index)] : nullptr; }

private:
	std::vector<Item> mItems;
};

inline SkColor ToSkColor(const glint_color& color)
{
	return SkColorSetARGB(color.A, color.R, color.G, color.B);
}

inline SkRect ToSkRect(const glint_rect& rect)
{
	return SkRect::MakeLTRB(rect.L, rect.T, rect.R, rect.B);
}

inline sk_sp<SkImage> LoadSkImageFromData(sk_sp<SkData> data)
{
	return data ? SkImages::DeferredFromEncodedData(std::move(data)) : nullptr;
}

inline glint_bitmap LoadBitmapFromData(sk_sp<SkData> data, int numFrames = 1)
{
	return glint_bitmap(LoadSkImageFromData(std::move(data)), numFrames);
}

inline glint_bitmap LoadBitmapFromFile(const char* fileNameOrPath, int numFrames = 1)
{
	if (!fileNameOrPath || !*fileNameOrPath) return {};
	return LoadBitmapFromData(SkData::MakeFromFileName(fileNameOrPath), numFrames);
}

inline bool ParseSVGIntrinsicSize(const void* svgBytes, size_t byteLen, float& outW, float& outH)
{
	if (!svgBytes || byteLen == 0) return false;
	const char* src = static_cast<const char*>(svgBytes);
	const char* end = src + std::min(byteLen, static_cast<size_t>(1024));

	auto findAttr = [&](const char* attrName, float& value) -> bool
	{
		const size_t nameLen = std::strlen(attrName);
		for (const char* p = src; p + nameLen + 2 < end; ++p)
		{
			if (std::strncmp(p, attrName, nameLen) != 0) continue;
			const char* q = p + nameLen;
			while (q < end && (*q == ' ' || *q == '\t')) ++q;
			if (q >= end || *q != '=') continue;
			++q;
			while (q < end && (*q == ' ' || *q == '\t')) ++q;
			if (q >= end) continue;
			const char quote = *q++;
			if (quote != '"' && quote != '\'') continue;
			char buffer[64] = {};
			int index = 0;
			bool hasPercent = false;
			while (q < end && *q != quote && index < 63)
			{
				if (*q == '%') hasPercent = true;
				buffer[index++] = *q++;
			}
			if (hasPercent) return false;
			value = static_cast<float>(std::atof(buffer));
			return value > 0.f;
		}
		return false;
	};

	float width = 0.f;
	float height = 0.f;
	if (findAttr("width", width) && findAttr("height", height))
	{
		outW = width;
		outH = height;
		return true;
	}
	return false;
}

inline glint_svg LoadSVGFromData(sk_sp<SkData> data)
{
	if (!data) return {};
	SkMemoryStream stream(data);
	auto dom = SkSVGDOM::Builder().make(stream);
	if (!dom) return {};
	if (dom->containerSize().width() == 0.f)
	{
		float svgW = 0.f;
		float svgH = 0.f;
		if (ParseSVGIntrinsicSize(data->data(), data->size(), svgW, svgH))
			dom->setContainerSize(SkSize::Make(svgW, svgH));
	}
	glint_svg svg(dom);
	svg.setSize(dom->containerSize());
	return svg;
}

inline glint_svg LoadSVGFromFile(const char* fileNameOrPath)
{
	if (!fileNameOrPath || !*fileNameOrPath) return {};
	return LoadSVGFromData(SkData::MakeFromFileName(fileNameOrPath));
}

class glint_canvas
{
public:
	explicit glint_canvas(void* drawContext = nullptr, void* windowHandle = nullptr)
		: mDrawContext(drawContext), mWindowHandle(windowHandle) {}

	void* GetDrawContext() const { return mDrawContext; }
	void* GetWindow() const { return mWindowHandle; }
	int Width() const { return mWidth; }
	int Height() const { return mHeight; }
#if defined(_WIN32)
	HMODULE GetWinModuleHandle() const { return ::GetModuleHandleW(nullptr); }
#endif

	void SetDrawContext(void* drawContext) { mDrawContext = drawContext; }
	void SetWindow(void* windowHandle) { mWindowHandle = windowHandle; }
	void SetSize(int width, int height) { mWidth = width; mHeight = height; }

	bool LoadFont(const char* /*fontID*/, const char* /*fileNameOrResID*/) { return false; }
	bool LoadFont(const char* /*fontID*/, void* /*data*/, int /*size*/) { return false; }

	glint_bitmap LoadBitmap(const char* fileNameOrPath, int numFrames = 1)
	{
		return LoadBitmapFromFile(fileNameOrPath, numFrames);
	}

	glint_bitmap LoadBitmap(const char* /*debugName*/, const void* data, int size, int numFrames = 1)
	{
		return LoadBitmapFromData(SkData::MakeWithCopy(data, static_cast<size_t>(size)), numFrames);
	}

	glint_svg LoadSVG(const char* fileNameOrPath)
	{
		return LoadSVGFromFile(fileNameOrPath);
	}

	glint_svg LoadSVG(const char* /*debugName*/, const void* data, int size)
	{
		return LoadSVGFromData(SkData::MakeWithCopy(data, static_cast<size_t>(size)));
	}

	void FillRect(glint_color color, const glint_rect& rect)
	{
		if (auto* canvas = static_cast<SkCanvas*>(mDrawContext))
		{
			SkPaint paint;
			paint.setAntiAlias(true);
			paint.setStyle(SkPaint::kFill_Style);
			paint.setColor(ToSkColor(color));
			canvas->drawRect(ToSkRect(rect), paint);
		}
	}

	void DrawRect(glint_color color, const glint_rect& rect, const void* /*unused*/ = nullptr, float width = 1.f)
	{
		if (auto* canvas = static_cast<SkCanvas*>(mDrawContext))
		{
			SkPaint paint;
			paint.setAntiAlias(true);
			paint.setStyle(SkPaint::kStroke_Style);
			paint.setStrokeWidth(width);
			paint.setColor(ToSkColor(color));
			canvas->drawRect(ToSkRect(rect), paint);
		}
	}

	void FillRoundRect(glint_color color, const glint_rect& rect, float radius)
	{
		if (auto* canvas = static_cast<SkCanvas*>(mDrawContext))
		{
			SkPaint paint;
			paint.setAntiAlias(true);
			paint.setStyle(SkPaint::kFill_Style);
			paint.setColor(ToSkColor(color));
			canvas->drawRoundRect(ToSkRect(rect), radius, radius, paint);
		}
	}

	void DrawRoundRect(glint_color color, const glint_rect& rect, float radius, const void* /*unused*/ = nullptr, float width = 1.f)
	{
		if (auto* canvas = static_cast<SkCanvas*>(mDrawContext))
		{
			SkPaint paint;
			paint.setAntiAlias(true);
			paint.setStyle(SkPaint::kStroke_Style);
			paint.setStrokeWidth(width);
			paint.setColor(ToSkColor(color));
			canvas->drawRoundRect(ToSkRect(rect), radius, radius, paint);
		}
	}

	void DrawLine(glint_color color, float x1, float y1, float x2, float y2, const void* /*unused*/ = nullptr, float width = 1.f)
	{
		if (auto* canvas = static_cast<SkCanvas*>(mDrawContext))
		{
			SkPaint paint;
			paint.setAntiAlias(true);
			paint.setStyle(SkPaint::kStroke_Style);
			paint.setStrokeWidth(width);
			paint.setColor(ToSkColor(color));
			canvas->drawLine(x1, y1, x2, y2, paint);
		}
	}

	void DrawDottedLine(glint_color color, float x1, float y1, float x2, float y2, const void* /*unused*/ = nullptr, float width = 1.f, float dash = 4.f)
	{
		if (auto* canvas = static_cast<SkCanvas*>(mDrawContext))
		{
			SkPaint paint;
			paint.setAntiAlias(true);
			paint.setStyle(SkPaint::kStroke_Style);
			paint.setStrokeWidth(width);
			paint.setColor(ToSkColor(color));
			const SkScalar intervals[] = { dash, dash };
			paint.setPathEffect(SkDashPathEffect::Make(intervals, 2, 0.f));
			canvas->drawLine(x1, y1, x2, y2, paint);
		}
	}

	void DrawDottedRect(glint_color color, const glint_rect& rect, const void* /*unused*/ = nullptr, float width = 1.f, float dash = 4.f)
	{
		DrawDottedLine(color, rect.L, rect.T, rect.R, rect.T, nullptr, width, dash);
		DrawDottedLine(color, rect.R, rect.T, rect.R, rect.B, nullptr, width, dash);
		DrawDottedLine(color, rect.R, rect.B, rect.L, rect.B, nullptr, width, dash);
		DrawDottedLine(color, rect.L, rect.B, rect.L, rect.T, nullptr, width, dash);
	}

	void FillCircle(glint_color color, float cx, float cy, float radius)
	{
		if (auto* canvas = static_cast<SkCanvas*>(mDrawContext))
		{
			SkPaint paint;
			paint.setAntiAlias(true);
			paint.setStyle(SkPaint::kFill_Style);
			paint.setColor(ToSkColor(color));
			canvas->drawCircle(cx, cy, radius, paint);
		}
	}

	void DrawCircle(glint_color color, float cx, float cy, float radius, const void* /*unused*/ = nullptr, float width = 1.f)
	{
		if (auto* canvas = static_cast<SkCanvas*>(mDrawContext))
		{
			SkPaint paint;
			paint.setAntiAlias(true);
			paint.setStyle(SkPaint::kStroke_Style);
			paint.setStrokeWidth(width);
			paint.setColor(ToSkColor(color));
			canvas->drawCircle(cx, cy, radius, paint);
		}
	}

	void FillConvexPolygon(glint_color color, const float* xs, const float* ys, int count)
	{
		if (auto* canvas = static_cast<SkCanvas*>(mDrawContext))
		{
			SkPath path;
			if (count > 0)
			{
				path.moveTo(xs[0], ys[0]);
				for (int i = 1; i < count; ++i) path.lineTo(xs[i], ys[i]);
				path.close();
			}
			SkPaint paint;
			paint.setAntiAlias(true);
			paint.setStyle(SkPaint::kFill_Style);
			paint.setColor(ToSkColor(color));
			canvas->drawPath(path, paint);
		}
	}

	void DrawConvexPolygon(glint_color color, const float* xs, const float* ys, int count, const void* /*unused*/ = nullptr, float width = 1.f)
	{
		if (auto* canvas = static_cast<SkCanvas*>(mDrawContext))
		{
			SkPath path;
			if (count > 0)
			{
				path.moveTo(xs[0], ys[0]);
				for (int i = 1; i < count; ++i) path.lineTo(xs[i], ys[i]);
				path.close();
			}
			SkPaint paint;
			paint.setAntiAlias(true);
			paint.setStyle(SkPaint::kStroke_Style);
			paint.setStrokeWidth(width);
			paint.setColor(ToSkColor(color));
			canvas->drawPath(path, paint);
		}
	}

	void PathClear() { mPath = SkPath{}; }
	void PathSetWinding(bool winding) { mPath.setFillType(winding ? SkPathFillType::kWinding : SkPathFillType::kEvenOdd); }
	void PathMoveTo(float x, float y) { mPath.moveTo(x, y); }
	void PathLineTo(float x, float y) { mPath.lineTo(x, y); }
	void PathClose() { mPath.close(); }

	void PathFill(glint_color color, const glint_fill_options& options)
	{
		if (auto* canvas = static_cast<SkCanvas*>(mDrawContext))
		{
			SkPath path = mPath;
			path.setFillType(options.fillRule == EFillRule::Preserve ? SkPathFillType::kEvenOdd : SkPathFillType::kWinding);
			SkPaint paint;
			paint.setAntiAlias(true);
			paint.setStyle(SkPaint::kFill_Style);
			paint.setColor(ToSkColor(color));
			canvas->drawPath(path, paint);
			if (!options.preserve) mPath = SkPath{};
		}
	}

	void DrawText(const glint_text& text, const char* utf8, const glint_rect& rect)
	{
		if (auto* canvas = static_cast<SkCanvas*>(mDrawContext))
		{
			sk_sp<SkFontMgr> mgr;
#if defined(SK_BUILD_FOR_WIN)
			mgr = SkFontMgr_New_DirectWrite();
#elif defined(SK_BUILD_FOR_MAC) || defined(SK_BUILD_FOR_IOS)
			mgr = SkFontMgr_New_CoreText(nullptr);
#else
			mgr = SkFontMgr::RefEmpty();
#endif
			sk_sp<SkTypeface> typeface;
			if (mgr && !text.mFont.empty()) typeface = mgr->legacyMakeTypeface(text.mFont.c_str(), SkFontStyle::Normal());
			SkFont font(typeface, text.mSize > 0.f ? text.mSize : 12.f);
			font.setSubpixel(true);
			font.setEdging(SkFont::Edging::kSubpixelAntiAlias);
			const std::string value = utf8 ? utf8 : "";
			SkRect bounds;
			const float advance = value.empty() ? 0.f : font.measureText(value.c_str(), value.size(), SkTextEncoding::kUTF8, &bounds);
			float x = rect.L;
			if (text.mAlign == EAlign::Center) x = rect.L + (rect.W() - advance) * 0.5f;
			else if (text.mAlign == EAlign::Far) x = rect.R - advance;
			SkFontMetrics metrics;
			font.getMetrics(&metrics);
			float y = rect.T - metrics.fAscent;
			if (text.mVAlign == EVAlign::Middle)
				y = rect.T + rect.H() * 0.5f - (metrics.fAscent + metrics.fDescent) * 0.5f;
			else if (text.mVAlign == EVAlign::Bottom)
				y = rect.B - metrics.fDescent;
			SkPaint paint;
			paint.setAntiAlias(true);
			paint.setColor(ToSkColor(text.mFGColor));
			canvas->drawString(value.c_str(), x, y, font, paint);
		}
	}

	void DrawFittedBitmap(const glint_bitmap& bitmap, const glint_rect& dest)
	{
		if (auto* canvas = static_cast<SkCanvas*>(mDrawContext))
		{
			if (auto* api = bitmap.GetAPIBitmap())
			{
				auto image = api->image();
				if (!image) return;
				canvas->drawImageRect(image, ToSkRect(dest), SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kLinear), nullptr);
			}
		}
	}

	void DrawBitmap(const glint_bitmap& bitmap, const glint_rect& dest, int /*srcX*/, int /*srcY*/)
	{
		DrawFittedBitmap(bitmap, dest);
	}

	void DrawSVG(const glint_svg& svg, const glint_rect& dest, const void* /*unused*/ = nullptr,
							 const glint_color* /*stroke*/ = nullptr, const glint_color* /*fill*/ = nullptr)
	{
		if (auto* canvas = static_cast<SkCanvas*>(mDrawContext))
		{
			auto dom = svg.dom();
			if (!dom) return;
			canvas->save();
			canvas->translate(dest.L, dest.T);
			const SkSize natural = dom->containerSize();
			const float srcW = natural.width() > 0.f ? natural.width() : std::max(1.f, dest.W());
			const float srcH = natural.height() > 0.f ? natural.height() : std::max(1.f, dest.H());
			dom->setContainerSize(SkSize::Make(dest.W(), dest.H()));
			canvas->scale(dest.W() / srcW, dest.H() / srcH);
			dom->render(canvas);
			canvas->restore();
		}
	}

private:
	void* mDrawContext = nullptr;
	void* mWindowHandle = nullptr;
	int mWidth = 0;
	int mHeight = 0;
	SkPath mPath;
};

} // namespace glint_graphics
