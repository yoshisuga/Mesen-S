#include "stdafx.h"
#include "PpuTools.h"
#include "Ppu.h"
#include "DebugTypes.h"
#include "Console.h"
#include "BaseCartridge.h"
#include "MemoryManager.h"
#include "NotificationManager.h"

PpuTools::PpuTools(Console *console, Ppu *ppu)
{
	_console = console;
	_ppu = ppu;
}

uint8_t PpuTools::GetTilePixelColor(const uint8_t* ram, const uint32_t ramMask, const uint8_t bpp, const uint32_t pixelStart, const uint8_t shift)
{
	uint8_t color;
	if(bpp == 2) {
		color = (((ram[(pixelStart + 0) & ramMask] >> shift) & 0x01) << 0);
		color |= (((ram[(pixelStart + 1) & ramMask] >> shift) & 0x01) << 1);
	} else if(bpp == 4) {
		color = (((ram[(pixelStart + 0) & ramMask] >> shift) & 0x01) << 0);
		color |= (((ram[(pixelStart + 1) & ramMask] >> shift) & 0x01) << 1);
		color |= (((ram[(pixelStart + 16) & ramMask] >> shift) & 0x01) << 2);
		color |= (((ram[(pixelStart + 17) & ramMask] >> shift) & 0x01) << 3);
	} else if(bpp == 8) {
		color = (((ram[(pixelStart + 0) & ramMask] >> shift) & 0x01) << 0);
		color |= (((ram[(pixelStart + 1) & ramMask] >> shift) & 0x01) << 1);
		color |= (((ram[(pixelStart + 16) & ramMask] >> shift) & 0x01) << 2);
		color |= (((ram[(pixelStart + 17) & ramMask] >> shift) & 0x01) << 3);
		color |= (((ram[(pixelStart + 32) & ramMask] >> shift) & 0x01) << 4);
		color |= (((ram[(pixelStart + 33) & ramMask] >> shift) & 0x01) << 5);
		color |= (((ram[(pixelStart + 48) & ramMask] >> shift) & 0x01) << 6);
		color |= (((ram[(pixelStart + 49) & ramMask] >> shift) & 0x01) << 7);
	} else {
		throw std::runtime_error("unsupported bpp");
	}
	return color;
}

uint32_t PpuTools::ToArgb(uint16_t color)
{
	uint8_t b = (color >> 10) << 3;
	uint8_t g = ((color >> 5) & 0x1F) << 3;
	uint8_t r = (color & 0x1F) << 3;

	return 0xFF000000 | (r << 16) | (g << 8) | b;
}

void PpuTools::BlendColors(uint8_t output[4], uint8_t input[4])
{
	uint8_t alpha = input[3] + 1;
	uint8_t invertedAlpha = 256 - input[3];
	output[0] = (uint8_t)((alpha * input[0] + invertedAlpha * output[0]) >> 8);
	output[1] = (uint8_t)((alpha * input[1] + invertedAlpha * output[1]) >> 8);
	output[2] = (uint8_t)((alpha * input[2] + invertedAlpha * output[2]) >> 8);
	output[3] = 0xFF;
}

uint32_t PpuTools::GetRgbPixelColor(uint8_t* cgram, uint8_t colorIndex, uint8_t palette, uint8_t bpp, bool directColorMode, uint16_t basePaletteOffset)
{
	uint16_t paletteColor;
	if(bpp == 8 && directColorMode) {
		paletteColor = (
			((((colorIndex & 0x07) << 1) | (palette & 0x01)) << 1) |
			(((colorIndex & 0x38) | ((palette & 0x02) << 1)) << 4) |
			(((colorIndex & 0xC0) | ((palette & 0x04) << 3)) << 7)
		);
	} else {
		uint16_t paletteRamOffset = basePaletteOffset + (palette * (1 << bpp) + colorIndex) * 2;
		paletteColor = cgram[paletteRamOffset] | (cgram[paletteRamOffset + 1] << 8);
	}
	return ToArgb(paletteColor);
}

void PpuTools::GetTileView(GetTileViewOptions options, uint8_t *source, uint32_t srcSize, uint8_t *cgram, uint32_t *outBuffer)
{
	uint8_t* ram = source;
	uint32_t ramMask = srcSize - 1;
	uint8_t bpp;

	bool directColor = false;
	switch(options.Format) {
		case TileFormat::Bpp2: bpp = 2; break;
		case TileFormat::Bpp4: bpp = 4; break;

		case TileFormat::DirectColor:
			directColor = true;
			bpp = 8;
			break;

		case TileFormat::Mode7:
			bpp = 16;
			break;

		case TileFormat::Mode7DirectColor:
			directColor = true;
			bpp = 16;
			break;

		default: bpp = 8; break;
	}

	int bytesPerTile = 64 * bpp / 8;
	int tileCount = 0x10000 / bytesPerTile;
	
	uint16_t bgColor = (cgram[1] << 8) | cgram[0];
	for(int i = 0; i < 512 * 512; i++) {
		outBuffer[i] = ToArgb(bgColor);
	}

	int rowCount = tileCount / options.Width;

	for(int row = 0; row < rowCount; row++) {
		uint32_t baseOffset = row * bytesPerTile * options.Width;

		for(int column = 0; column < options.Width; column++) {
			uint32_t addr = baseOffset + bytesPerTile * column;

			int baseOutputOffset;				
			if(options.Layout == TileLayout::SingleLine8x16) {
				int displayColumn = column / 2 + ((row & 0x01) ? options.Width/2 : 0);
				int displayRow = (row & ~0x01) + ((column & 0x01) ? 1 : 0);
				baseOutputOffset = displayRow * options.Width * 64 + displayColumn * 8;
			} else if(options.Layout == TileLayout::SingleLine16x16) {
				int displayColumn = (column / 2) + (column & 0x01) + ((row & 0x01) ? options.Width/2 : 0) + ((column & 0x02) ? -1 : 0);
				int displayRow = (row & ~0x01) + ((column & 0x02) ? 1 : 0);
				baseOutputOffset = displayRow * options.Width * 64 + displayColumn * 8;
			} else {
				baseOutputOffset = row * options.Width * 64 + column * 8;
			}

			if(options.Format == TileFormat::Mode7 || options.Format == TileFormat::Mode7DirectColor) {
				for(int y = 0; y < 8; y++) {
					uint32_t pixelStart = addr + y * 16;

					for(int x = 0; x < 8; x++) {
						uint8_t color = ram[(pixelStart + x * 2 + 1) & ramMask];

						if(color != 0) {
							uint32_t rgbColor;
							if(directColor) {
								rgbColor = ToArgb(((color & 0x07) << 2) | ((color & 0x38) << 4) | ((color & 0xC0) << 7));
							} else {
								rgbColor = GetRgbPixelColor(cgram, color, 0, 8, false, 0);
							}
							outBuffer[baseOutputOffset + (y*options.Width*8) + x] = rgbColor;
						}
					}
				}
			} else {
				for(int y = 0; y < 8; y++) {
					uint32_t pixelStart = addr + y * 2;
					for(int x = 0; x < 8; x++) {
						uint8_t color = GetTilePixelColor(ram, ramMask, bpp, pixelStart, 7 - x);
						if(color != 0) {
							outBuffer[baseOutputOffset + (y*options.Width*8) + x] = GetRgbPixelColor(cgram, color, options.Palette, bpp, directColor, 0);
						}
					}
				}
			}
		}
	}
}

void PpuTools::GetTilemap(GetTilemapOptions options, PpuState state, uint8_t* vram, uint8_t* cgram, uint32_t* outBuffer)
{
	static constexpr uint8_t layerBpp[8][4] = {
		{ 2,2,2,2 }, { 4,4,2,0 }, { 4,4,0,0 }, { 8,4,0,0 }, { 8,2,0,0 }, { 4,2,0,0 }, { 4,0,0,0 }, { 8,0,0,0 }
	};

	bool directColor = state.DirectColorMode && (state.BgMode == 3 || state.BgMode == 4 || state.BgMode == 7);

	uint16_t basePaletteOffset = 0;
	if(state.BgMode == 0) {
		basePaletteOffset = options.Layer * 64;
	}

	LayerConfig layer = state.Layers[options.Layer];

	uint32_t bgColor = ToArgb((cgram[1] << 8) | cgram[0]);
	std::fill(outBuffer, outBuffer + 1024*1024, bgColor);

	uint8_t bpp = layerBpp[state.BgMode][options.Layer];
	if(bpp == 0) {
		return;
	}

	bool largeTileWidth = layer.LargeTiles || state.BgMode == 5 || state.BgMode == 6;
	bool largeTileHeight = layer.LargeTiles;

	if(state.BgMode == 7) {
		for(int row = 0; row < 128; row++) {
			for(int column = 0; column < 128; column++) {
				uint32_t tileIndex = vram[row * 256 + column * 2];
				uint32_t tileAddr = tileIndex * 128;

				for(int y = 0; y < 8; y++) {
					uint32_t pixelStart = tileAddr + y * 16;

					for(int x = 0; x < 8; x++) {
						uint8_t color = vram[pixelStart + x * 2 + 1];

						if(color != 0) {
							uint32_t rgbColor;
							if(directColor) {
								rgbColor = ToArgb(((color & 0x07) << 2) | ((color & 0x38) << 4) | ((color & 0xC0) << 7));
							} else {
								rgbColor = GetRgbPixelColor(cgram, color, 0, 8, false, 0);
							}
							outBuffer[((row * 8) + y) * 1024 + column * 8 + x] = rgbColor;
						}
					}
				}
			}
		}
	} else {
		int tileHeight = largeTileHeight ? 16 : 8;
		int tileWidth = largeTileWidth ? 16 : 8;
		for(int row = 0; row < (layer.DoubleHeight ? 64 : 32); row++) {
			uint16_t addrVerticalScrollingOffset = layer.DoubleHeight ? ((row & 0x20) << (layer.DoubleWidth ? 6 : 5)) : 0;
			uint16_t baseOffset = layer.TilemapAddress + addrVerticalScrollingOffset + ((row & 0x1F) << 5);

			for(int column = 0; column < (layer.DoubleWidth ? 64 : 32); column++) {
				uint16_t addr = (baseOffset + (column & 0x1F) + (layer.DoubleWidth ? ((column & 0x20) << 5) : 0)) << 1;

				bool vMirror = (vram[addr + 1] & 0x80) != 0;
				bool hMirror = (vram[addr + 1] & 0x40) != 0;
				uint16_t tileIndex = ((vram[addr + 1] & 0x03) << 8) | vram[addr];

				for(int y = 0; y < tileHeight; y++) {
					uint8_t yOffset = vMirror ? (7 - (y & 0x07)) : (y & 0x07);

					for(int x = 0; x < tileWidth; x++) {
						uint16_t tileOffset = (
							(largeTileHeight ? ((y & 0x08) ? (vMirror ? 0 : 16) : (vMirror ? 16 : 0)) : 0) +
							(largeTileWidth ? ((x & 0x08) ? (hMirror ? 0 : 1) : (hMirror ? 1 : 0)) : 0)
						);

						uint16_t tileStart = (layer.ChrAddress << 1) + ((tileIndex + tileOffset) & 0x3FF) * 8 * bpp;
						uint16_t pixelStart = tileStart + yOffset * 2;

						uint8_t shift = hMirror ? (x & 0x07) : (7 - (x & 0x07));
						uint8_t color = GetTilePixelColor(vram, Ppu::VideoRamSize - 1, bpp, pixelStart, shift);
						if(color != 0) {
							uint8_t palette = bpp == 8 ? 0 : (vram[addr + 1] >> 2) & 0x07;
							outBuffer[((row * tileHeight) + y) * 1024 + column * tileWidth + x] = GetRgbPixelColor(cgram, color, palette, bpp, directColor, basePaletteOffset);
						}
					}
				}
			}
		}
	}
}

static constexpr uint8_t _oamSizes[8][2][2] = {
	{ { 1, 1 }, { 2, 2 } }, //8x8 + 16x16
	{ { 1, 1 }, { 4, 4 } }, //8x8 + 32x32
	{ { 1, 1 }, { 8, 8 } }, //8x8 + 64x64
	{ { 2, 2 }, { 4, 4 } }, //16x16 + 32x32
	{ { 2, 2 }, { 8, 8 } }, //16x16 + 64x64
	{ { 4, 4 }, { 8, 8 } }, //32x32 + 64x64
	{ { 2, 4 }, { 4, 8 } }, //16x32 + 32x64
	{ { 2, 4 }, { 4, 4 } }  //16x32 + 32x32
};

void PpuTools::GetSpritePreview(GetSpritePreviewOptions options, PpuState state, uint8_t *vram, uint8_t *oamRam, uint8_t *cgram, uint32_t *outBuffer)
{
	//TODO
	//uint16_t baseAddr = state.EnableOamPriority ? (_internalOamAddress & 0x1FC) : 0;
	uint16_t baseAddr = 0;

	bool filled[256 * 240] = {};
	int lastScanline = state.OverscanMode ? 239 : 224;
	std::fill(outBuffer, outBuffer + 256 * lastScanline, 0xFF888888);
	std::fill(outBuffer + 256 * lastScanline, outBuffer + 256 * 240, 0xFF000000);

	for(int screenY = 0; screenY < lastScanline; screenY++) {
		for(int i = 508; i >= 0; i -= 4) {
			uint16_t addr = (baseAddr + i) & 0x1FF;
			uint8_t y = oamRam[addr + 1];

			uint8_t highTableOffset = addr >> 4;
			uint8_t shift = ((addr >> 2) & 0x03) << 1;
			uint8_t highTableValue = oamRam[0x200 | highTableOffset] >> shift;
			uint8_t largeSprite = (highTableValue & 0x02) >> 1;
			uint8_t height = _oamSizes[state.OamMode][largeSprite][1] << 3;

			if(state.ObjInterlace) {
				height /= 2;
			}

			uint8_t endY = (y + height) & 0xFF;

			bool visible = (screenY >= y && screenY < endY) || (endY < y && screenY < endY);
			if(!visible) {
				//Not visible on this scanline
				continue;
			}

			uint8_t width = _oamSizes[state.OamMode][largeSprite][0] << 3;
			uint16_t sign = (highTableValue & 0x01) << 8;
			int16_t x = (int16_t)((sign | oamRam[addr]) << 7) >> 7;

			if(x != -256 && (x + width <= 0 || x > 255)) {
				//Sprite is not visible (and must be ignored for time/range flag calculations)
				//Sprites at X=-256 are always used when considering Time/Range flag calculations, but not actually drawn.
				continue;
			}

			int tileRow = (oamRam[addr + 2] & 0xF0) >> 4;
			int tileColumn = oamRam[addr + 2] & 0x0F;

			uint8_t flags = oamRam[addr + 3];
			bool useSecondTable = (flags & 0x01) != 0;
			uint8_t palette = (flags >> 1) & 0x07;
			//uint8_t priority = (flags >> 4) & 0x03;
			bool horizontalMirror = (flags & 0x40) != 0;
			bool verticalMirror = (flags & 0x80) != 0;
			
			uint8_t yOffset;
			int rowOffset;

			int yGap = (screenY - y);
			if(state.ObjInterlace) {
				yGap <<= 1;
			}
			if(verticalMirror) {
				yOffset = (height - 1 - yGap) & 0x07;
				rowOffset = (height - 1 - yGap) >> 3;
			} else {
				yOffset = yGap & 0x07;
				rowOffset = yGap >> 3;
			}

			uint8_t row = (tileRow + rowOffset) & 0x0F;

			for(int j = std::max<int16_t>(x, 0); j < x + width && j < 256; j++) {
				uint32_t outOffset = screenY * 256 + j;
				if(filled[outOffset]) {
					continue;
				}

				uint8_t xOffset;
				int columnOffset;
				if(horizontalMirror) {
					xOffset = (width - (j - x) - 1) & 0x07;
					columnOffset = (width - (j - x) - 1) >> 3;
				} else {
					xOffset = (j - x) & 0x07;
					columnOffset = (j - x) >> 3;
				}

				uint8_t column = (tileColumn + columnOffset) & 0x0F;
				uint8_t tileIndex = (row << 4) | column;
				uint16_t tileStart = ((state.OamBaseAddress + (tileIndex << 4) + (useSecondTable ? state.OamAddressOffset : 0)) & 0x7FFF) << 1;

				uint8_t color = GetTilePixelColor(vram, Ppu::VideoRamSize - 1, 4, tileStart + yOffset * 2, 7 - xOffset);
				if(color != 0) {
					if(options.SelectedSprite == i / 4) {
						filled[outOffset] = true;
					}
					outBuffer[outOffset] = GetRgbPixelColor(cgram, color, palette, 4, false, 256);
				}
			}
		}
	}
}

void PpuTools::SetViewerUpdateTiming(uint32_t viewerId, uint16_t scanline, uint16_t cycle)
{
	//TODO Thread safety
	_updateTimings[viewerId] = (scanline << 16) | cycle;
}

void PpuTools::RemoveViewer(uint32_t viewerId)
{
	//TODO Thread safety
	_updateTimings.erase(viewerId);
}

void PpuTools::UpdateViewers(uint16_t scanline, uint16_t cycle)
{
	uint32_t currentCycle = (scanline << 16) | cycle;
	for(auto updateTiming : _updateTimings) {
		if(updateTiming.second == currentCycle) {
			_console->GetNotificationManager()->SendNotification(ConsoleNotificationType::ViewerRefresh, (void*)(uint64_t)updateTiming.first);
		}
	}
}