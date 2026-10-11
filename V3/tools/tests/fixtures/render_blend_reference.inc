// Unmodified V3 blend routines captured before the palette-pointer optimization.
// gameplaySP, GPL-2.0-or-later; see gbsp/components/gbsp-libretro/video.cpp.
#define BLND_MSK (SATR_MSK | SATG_MSK | SATB_MSK)

#ifdef USE_XBGR1555_FORMAT
  #define OVFG_MSK 0x04000000
  #define OVFR_MSK 0x00008000
  #define OVFB_MSK 0x00000020
  #define SATG_MSK 0x03E00000
  #define SATR_MSK 0x00007C00
  #define SATB_MSK 0x0000001F
#else
  #define OVFG_MSK 0x08000000
  #define OVFR_MSK 0x00010000
  #define OVFB_MSK 0x00000020
  #define SATG_MSK 0x07E00000
  #define SATR_MSK 0x0000F800
  #define SATB_MSK 0x0000001F
#endif

typedef enum
{
  OBJ_BLEND,    // No effects, just blend forced-blend pixels (ie. ST objects)
  BLEND_ONLY,   // Just alpha blending (if the pixels are 1st and 2nd target)
  BLEND_BRIGHT, // Perform alpha blending if appropiate, and brighten otherwise
  BLEND_DARK,   // Same but with darken effecg
} blendtype;

// Applies blending (and optional brighten/darken) effect to a bunch of
// color-indexed pixel pairs. Depending on the mode and the pixel target
// number, blending, darken/brighten or no effect will be applied.
// Bits 0-8 encode the color index (paletted colors)
// Bit 9 is set if the pixel belongs to a 1st target layer
// Bit 10 is set if the pixel belongs to a 2nd target layer
// Bit 11 is set if the pixel belongs to a ST-object
template <blendtype bldtype, bool st_objs>
static void merge_blend(u32 start, u32 end, u16 *dst, u32 *src) {
  u32 bldalpha = read_ioreg(REG_BLDALPHA);
  u32 brightf = MIN(16, read_ioreg(REG_BLDY) & 0x1F);
  u32 blend_a = MIN(16, (bldalpha >> 0) & 0x1F);
  u32 blend_b = MIN(16, (bldalpha >> 8) & 0x1F);

  bool can_saturate = blend_a + blend_b > 16;

  if (can_saturate) {
    // If blending can result in saturation, we need to clamp output values.
    while (start < end) {
      u32 pixpair = src[start];
      // If ST-OBJ, force blending mode (has priority over other effects).
      // If regular blending mode, blend if 1st/2nd bits are set respectively.
      // Otherwise, apply other color effects if 1st bit is set.
      bool force_blend = (pixpair & 0x04000800) == 0x04000800;
      bool do_blend    = (pixpair & 0x04000200) == 0x04000200;
      if ((st_objs && force_blend) || (do_blend && bldtype == BLEND_ONLY)) {
        // Top pixel is 1st target, pixel below is 2nd target. Blend!
        u16 p1 = palette_ram_converted[(pixpair >>  0) & 0x1FF];
        u16 p2 = palette_ram_converted[(pixpair >> 16) & 0x1FF];
        u32 p1e = (p1 | (p1 << 16)) & BLND_MSK;
        u32 p2e = (p2 | (p2 << 16)) & BLND_MSK;
        u32 pfe = (((p1e * blend_a) + (p2e * blend_b)) >> 4);

        // If the overflow bit is set, saturate (set) all bits to one.
        if (pfe & (OVFR_MSK | OVFG_MSK | OVFB_MSK)) {
          if (pfe & OVFG_MSK)
            pfe |= SATG_MSK;
          if (pfe & OVFR_MSK)
            pfe |= SATR_MSK;
          if (pfe & OVFB_MSK)
            pfe |= SATB_MSK;
        }
        pfe &= BLND_MSK;
        dst[start++] = (pfe >> 16) | pfe;
      }
      else if ((bldtype == BLEND_DARK || bldtype == BLEND_BRIGHT) &&
               (pixpair & 0x200) == 0x200) {
        // Top pixel is 1st-target, can still apply bright/dark effect.
        u16 pidx = palette_ram_converted[pixpair & 0x1FF];
        u32 epixel = (pidx | (pidx << 16)) & BLND_MSK;
        u32 pa = bldtype == BLEND_DARK ? 0 : ((BLND_MSK * brightf) >> 4) & BLND_MSK;
        u32 pb = ((epixel * (16 - brightf)) >> 4) & BLND_MSK;
        epixel = (pa + pb) & BLND_MSK;
        dst[start++] = (epixel >> 16) | epixel;
      }
      else {
        dst[start++] = palette_ram_converted[pixpair & 0x1FF];   // No effects
      }
    }
  } else {
    while (start < end) {
      u32 pixpair = src[start];
      bool do_blend    = (pixpair & 0x04000200) == 0x04000200;
      bool force_blend = (pixpair & 0x04000800) == 0x04000800;
      if ((st_objs && force_blend) || (do_blend && bldtype == BLEND_ONLY)) {
        // Top pixel is 1st target, pixel below is 2nd target. Blend!
        u16 p1 = palette_ram_converted[(pixpair >>  0) & 0x1FF];
        u16 p2 = palette_ram_converted[(pixpair >> 16) & 0x1FF];
        u32 p1e = (p1 | (p1 << 16)) & BLND_MSK;
        u32 p2e = (p2 | (p2 << 16)) & BLND_MSK;
        u32 pfe = (((p1e * blend_a) + (p2e * blend_b)) >> 4) & BLND_MSK;
        dst[start++] = (pfe >> 16) | pfe;
      }
      else if ((bldtype == BLEND_DARK || bldtype == BLEND_BRIGHT) &&
               (pixpair & 0x200) == 0x200) {
        // Top pixel is 1st-target, can still apply bright/dark effect.
        u16 pidx = palette_ram_converted[pixpair & 0x1FF];
        u32 epixel = (pidx | (pidx << 16)) & BLND_MSK;
        u32 pa = bldtype == BLEND_DARK ? 0 : ((BLND_MSK * brightf) >> 4) & BLND_MSK;
        u32 pb = ((epixel * (16 - brightf)) >> 4) & BLND_MSK;
        epixel = (pa + pb) & BLND_MSK;
        dst[start++] = (epixel >> 16) | epixel;
      }
      else {
        dst[start++] = palette_ram_converted[pixpair & 0x1FF];   // No effects
      }
    }
  }
}

// Applies brighten/darken effect to a bunch of color-indexed pixels.
template <blendtype bldtype>
static void merge_brightness(u32 start, u32 end, u16 *srcdst) {
  u32 brightness = MIN(16, read_ioreg(REG_BLDY) & 0x1F);

  while (start < end) {
    u16 spix = srcdst[start];
    u16 pixcol = palette_ram_converted[spix & 0x1FF];

    if ((spix & 0x200) == 0x200) {
      // Pixel is 1st target, can apply color effect.
      u32 epixel = (pixcol | (pixcol << 16)) & BLND_MSK;
      u32 pa = bldtype == BLEND_DARK ? 0 : ((BLND_MSK * brightness) >> 4) & BLND_MSK; // B/W
      u32 pb = ((epixel * (16 - brightness)) >> 4) & BLND_MSK;  // Pixel color
      epixel = (pa + pb) & BLND_MSK;
      pixcol = (epixel >> 16) | epixel;
    }

    srcdst[start++] = pixcol;
  }
}

