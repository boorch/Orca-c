#include "gbuffer.h"
#include "mark.h"
#include "sim.h"

//////// Utilities

static Glyph const indexed_glyphs[] = {
    '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd',
    'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r',
    's', 't', 'u', 'v', 'w', 'x', 'y', 'z', '.', '*', ':', ';', '#',
};

enum { Glyphs_array_num = sizeof indexed_glyphs };

static inline Glyph glyph_lowered(Glyph c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c - ('a' - 'A')) : c;
}

// Always returns 0 through (sizeof indexed_glyphs) - 1, and works on
// capitalized glyphs as well. The index of the lower-cased glyph is returned
// if the glyph is capitalized.
static inline Usz semantic_index_of_glyph(Glyph c) {
  Glyph c0 = glyph_lowered(c);
  if (c0 == '.')
    return 0;
  for (Usz i = 0; i < Glyphs_array_num; ++i) {
    if (indexed_glyphs[i] == c0)
      return i;
  }
  return 0;
}

static inline Glyph glyphs_sum(Glyph a, Glyph b) {
  Usz ia = semantic_index_of_glyph(a);
  Usz ib = semantic_index_of_glyph(b);
  return indexed_glyphs[(ia + ib) % Glyphs_array_num];
}

static inline Glyph glyphs_mod(Glyph a, Glyph b) {
  Usz ia = semantic_index_of_glyph(a);
  Usz ib = semantic_index_of_glyph(b);
  return indexed_glyphs[ib == 0 ? 0 : (ia % ib)];
}

static inline bool oper_has_neighboring_bang(Gbuffer gbuf, Usz h, Usz w, Usz y,
                                             Usz x) {
  return gbuffer_peek_relative(gbuf, h, w, y, x, 0, 1) == '*' ||
         gbuffer_peek_relative(gbuf, h, w, y, x, 0, -1) == '*' ||
         gbuffer_peek_relative(gbuf, h, w, y, x, 1, 0) == '*' ||
         gbuffer_peek_relative(gbuf, h, w, y, x, -1, 0) == '*';
}

static inline void oper_move_relative_or_explode(Gbuffer gbuf, Mbuffer mbuf,
                                                 Usz height, Usz width,
                                                 Glyph moved, Usz y, Usz x,
                                                 Isz delta_y, Isz delta_x) {
  Isz y0 = (Isz)y + delta_y;
  Isz x0 = (Isz)x + delta_x;
  if (y0 >= (Isz)height || x0 >= (Isz)width || y0 < 0 || x0 < 0) {
    gbuf[y * width + x] = '*';
    return;
  }
  Glyph* at_dest = gbuf + (Usz)y0 * width + (Usz)x0;
  if (*at_dest != '.') {
    gbuf[y * width + x] = '*';
    mbuffer_poke_flags_or(mbuf, height, width, y, x, Mark_flag_sleep);
    return;
  }
  *at_dest = moved;
  mbuffer_poke_flags_or(mbuf, height, width, (Usz)y0, (Usz)x0, Mark_flag_sleep);
  gbuf[y * width + x] = '.';
}

#define ORCA_EXPAND_SOLO_OPER_CHARS(_oper_name, _oper_char)                    \
  Orca_oper_char_##_oper_name = _oper_char,
#define ORCA_EXPAND_DUAL_OPER_CHARS(_oper_name, _upper_oper_char,              \
                                    _lower_oper_char)                          \
  Orca_oper_upper_char_##_oper_name = _upper_oper_char,                        \
  Orca_oper_lower_char_##_oper_name = _lower_oper_char,
#define ORCA_DEFINE_OPER_CHARS(_solo_defs, _dual_defs)                         \
  enum Orca_oper_chars {                                                       \
    _solo_defs(ORCA_EXPAND_SOLO_OPER_CHARS)                                    \
        _dual_defs(ORCA_EXPAND_DUAL_OPER_CHARS)                                \
  };
#define ORCA_DECLARE_OPERATORS(_solo_defs, _dual_defs)                         \
  ORCA_DEFINE_OPER_CHARS(_solo_defs, _dual_defs)

#define OPER_IGNORE_COMMON_ARGS()                                              \
  (void)gbuffer;                                                               \
  (void)mbuffer;                                                               \
  (void)height;                                                                \
  (void)width;                                                                 \
  (void)y;                                                                     \
  (void)x;

#define BEGIN_SOLO_PHASE_0(_oper_name)                                         \
  static inline void oper_phase0_##_oper_name(                                 \
      Gbuffer const gbuffer, Mbuffer const mbuffer, Usz const height,          \
      Usz const width, Usz const y, Usz const x, U8 const cell_flags) {        \
    OPER_IGNORE_COMMON_ARGS()                                                  \
    (void)cell_flags;                                                          \
    enum { This_oper_char = Orca_oper_char_##_oper_name };
#define BEGIN_SOLO_PHASE_1(_oper_name)                                         \
  static inline void oper_phase1_##_oper_name(                                 \
      Gbuffer const gbuffer, Mbuffer const mbuffer, Usz const height,          \
      Usz const width, Usz const y, Usz const x) {                             \
    OPER_IGNORE_COMMON_ARGS()                                                  \
    enum { This_oper_char = Orca_oper_char_##_oper_name };
#define BEGIN_DUAL_PHASE_0(_oper_name)                                         \
  static inline void oper_phase0_##_oper_name(                                 \
      Gbuffer const gbuffer, Mbuffer const mbuffer, Usz const height,          \
      Usz const width, Usz const y, Usz const x, U8 const cell_flags,          \
      Glyph const This_oper_char) {                                            \
    OPER_IGNORE_COMMON_ARGS()                                                  \
    (void)cell_flags;                                                          \
    bool const Dual_is_uppercase =                                             \
        Orca_oper_upper_char_##_oper_name == This_oper_char;                   \
    (void)Dual_is_uppercase;
#define BEGIN_DUAL_PHASE_1(_oper_name)                                         \
  static inline void oper_phase1_##_oper_name(                                 \
      Gbuffer const gbuffer, Mbuffer const mbuffer, Usz const height,          \
      Usz const width, Usz const y, Usz const x, Glyph const This_oper_char) { \
    OPER_IGNORE_COMMON_ARGS()                                                  \
    bool const Dual_is_uppercase =                                             \
        Orca_oper_upper_char_##_oper_name == This_oper_char;                   \
    (void)Dual_is_uppercase;

#define END_PHASE }

#define OPER_POKE_ABSOLUTE(_y, _x, _glyph)                                     \
  gbuffer_poke(gbuffer, height, width, _y, _x, _glyph)
#define OPER_PEEK(_delta_y, _delta_x)                                          \
  gbuffer_peek_relative(gbuffer, height, width, y, x, _delta_y, _delta_x)
#define OPER_POKE(_delta_y, _delta_x, _glyph)                                  \
  gbuffer_poke_relative(gbuffer, height, width, y, x, _delta_y, _delta_x,      \
                        _glyph)
#define OPER_POKE_SELF(_glyph) OPER_POKE_ABSOLUTE(y, x, _glyph)

#define OPER_REQUIRE_BANG()                                                    \
  if (!oper_has_neighboring_bang(gbuffer, height, width, y, x))                \
  return

#define PORT_LOCKED Mark_flag_lock
#define PORT_UNLOCKED Mark_flag_none
#define PORT_HASTE Mark_flag_haste_input

#define REALIZE_DUAL                                                           \
  bool const Dual_is_active =                                                  \
      Dual_is_uppercase |                                                      \
      oper_has_neighboring_bang(gbuffer, height, width, y, x);

#define BEGIN_DUAL_PORTS                                                       \
  {                                                                            \
    bool const Oper_ports_enabled = Dual_is_active;

#define STOP_IF_DUAL_INACTIVE                                                  \
  if (!Dual_is_active)                                                         \
  return

#define I_PORT(_delta_y, _delta_x, _flags)                                     \
  mbuffer_poke_relative_flags_or(                                              \
      mbuffer, height, width, y, x, _delta_y, _delta_x,                        \
      Mark_flag_input | ((_flags)&Mark_flag_haste_input) |                     \
          (Oper_ports_enabled &&                                               \
                   (cell_flags & (Mark_flag_lock | Mark_flag_sleep))           \
               ? Mark_flag_none                                                \
               : (_flags)))
#define O_PORT(_delta_y, _delta_x, _flags)                                     \
  mbuffer_poke_relative_flags_or(                                              \
      mbuffer, height, width, y, x, _delta_y, _delta_x,                        \
      Mark_flag_input | ((_flags)&Mark_flag_haste_input) |                     \
          (Oper_ports_enabled &&                                               \
                   (cell_flags & (Mark_flag_lock | Mark_flag_sleep))           \
               ? Mark_flag_none                                                \
               : (_flags)))
#define END_PORTS }

#define BEGIN_HASTE if (!(cell_flags & (Mark_flag_lock | Mark_flag_sleep))) {
#define END_HASTE }

#define OPER_MOVE_OR_EXPLODE(_delta_y, _delta_x)                               \
  oper_move_relative_or_explode(gbuffer, mbuffer, height, width,               \
                                This_oper_char, y, x, _delta_y, _delta_x)

#define OPER_DEFINE_DIRECTIONAL(_oper_name, _delta_y, _delta_x)                \
  BEGIN_DUAL_PHASE_0(_oper_name)                                               \
    BEGIN_HASTE                                                                \
      REALIZE_DUAL;                                                            \
      STOP_IF_DUAL_INACTIVE;                                                   \
      OPER_MOVE_OR_EXPLODE(_delta_y, _delta_x);                                \
    END_HASTE                                                                  \
  END_PHASE                                                                    \
  BEGIN_DUAL_PHASE_1(_oper_name)                                               \
  END_PHASE

//////// Operators

#define ORCA_SOLO_OPERATORS(_) _(bang, '*')

#define ORCA_DUAL_OPERATORS(_)                                                 \
  _(north, 'N', 'n')                                                           \
  _(east, 'E', 'e')                                                            \
  _(south, 'S', 's')                                                           \
  _(west, 'W', 'w')                                                            \
  _(add, 'A', 'a')                                                             \
  _(modulo, 'M', 'm')                                                          \
  _(increment, 'I', 'i')

ORCA_DECLARE_OPERATORS(ORCA_SOLO_OPERATORS, ORCA_DUAL_OPERATORS)

//////// Behavior

OPER_DEFINE_DIRECTIONAL(north, -1, 0)
OPER_DEFINE_DIRECTIONAL(east, 0, 1)
OPER_DEFINE_DIRECTIONAL(south, 1, 0)
OPER_DEFINE_DIRECTIONAL(west, 0, -1)

BEGIN_DUAL_PHASE_0(add)
  REALIZE_DUAL;
  BEGIN_DUAL_PORTS
    I_PORT(0, 1, PORT_LOCKED);
    I_PORT(0, 2, PORT_LOCKED);
    O_PORT(1, 0, PORT_LOCKED);
  END_PORTS
END_PHASE
BEGIN_DUAL_PHASE_1(add)
  REALIZE_DUAL;
  STOP_IF_DUAL_INACTIVE;
  Glyph inp0 = OPER_PEEK(0, 1);
  Glyph inp1 = OPER_PEEK(0, 2);
  Glyph g = glyphs_sum(inp0, inp1);
  OPER_POKE(1, 0, g);
END_PHASE

BEGIN_DUAL_PHASE_0(modulo)
  REALIZE_DUAL;
  BEGIN_DUAL_PORTS
    I_PORT(0, 1, PORT_LOCKED);
    I_PORT(0, 2, PORT_LOCKED);
    O_PORT(1, 0, PORT_LOCKED);
  END_PORTS
END_PHASE
BEGIN_DUAL_PHASE_1(modulo)
  REALIZE_DUAL;
  STOP_IF_DUAL_INACTIVE;
  Glyph inp0 = OPER_PEEK(0, 1);
  Glyph inp1 = OPER_PEEK(0, 2);
  Glyph g = glyphs_mod(inp0, inp1);
  OPER_POKE(1, 0, g);
END_PHASE

BEGIN_DUAL_PHASE_0(increment)
  REALIZE_DUAL;
  BEGIN_DUAL_PORTS
    I_PORT(0, 1, PORT_LOCKED);
    I_PORT(0, 2, PORT_LOCKED);
    O_PORT(1, 0, PORT_LOCKED);
  END_PORTS
END_PHASE
BEGIN_DUAL_PHASE_1(increment)
END_PHASE

BEGIN_SOLO_PHASE_0(bang)
  BEGIN_HASTE
    OPER_POKE_SELF('.');
  END_HASTE
END_PHASE
BEGIN_SOLO_PHASE_1(bang)
END_PHASE

//////// Run simulation

#define SIM_EXPAND_SOLO_PHASE_0(_oper_name, _oper_char)                        \
  case _oper_char:                                                             \
    oper_phase0_##_oper_name(gbuf, mbuf, height, width, iy, ix, cell_flags);   \
    break;
#define SIM_EXPAND_SOLO_PHASE_1(_oper_name, _oper_char)                        \
  case _oper_char:                                                             \
    oper_phase1_##_oper_name(gbuf, mbuf, height, width, iy, ix);               \
    break;

#define SIM_EXPAND_DUAL_PHASE_0(_oper_name, _upper_oper_char,                  \
                                _lower_oper_char)                              \
  case _upper_oper_char:                                                       \
  case _lower_oper_char:                                                       \
    oper_phase0_##_oper_name(gbuf, mbuf, height, width, iy, ix, cell_flags,    \
                             glyph_char);                                      \
    break;
#define SIM_EXPAND_DUAL_PHASE_1(_oper_name, _upper_oper_char,                  \
                                _lower_oper_char)                              \
  case _upper_oper_char:                                                       \
  case _lower_oper_char:                                                       \
    oper_phase1_##_oper_name(gbuf, mbuf, height, width, iy, ix, glyph_char);   \
    break;

static void sim_phase_0(Gbuffer gbuf, Mbuffer mbuf, Usz height, Usz width) {
  for (Usz iy = 0; iy < height; ++iy) {
    Glyph* glyph_row = gbuf + iy * width;
    for (Usz ix = 0; ix < width; ++ix) {
      Glyph glyph_char = glyph_row[ix];
      if (glyph_char == '.')
        continue;
      U8 cell_flags = mbuffer_peek(mbuf, height, width, iy, ix) &
                      (Mark_flag_lock | Mark_flag_sleep);
      switch (glyph_char) {
        ORCA_SOLO_OPERATORS(SIM_EXPAND_SOLO_PHASE_0)
        ORCA_DUAL_OPERATORS(SIM_EXPAND_DUAL_PHASE_0)
      }
    }
  }
}

static void sim_phase_1(Gbuffer gbuf, Mbuffer mbuf, Usz height, Usz width) {
  for (Usz iy = 0; iy < height; ++iy) {
    Glyph* glyph_row = gbuf + iy * width;
    for (Usz ix = 0; ix < width; ++ix) {
      Glyph glyph_char = glyph_row[ix];
      if (glyph_char == '.')
        continue;
      if (mbuffer_peek(mbuf, height, width, iy, ix) &
          (Mark_flag_lock | Mark_flag_sleep))
        continue;
      switch (glyph_char) {
        ORCA_SOLO_OPERATORS(SIM_EXPAND_SOLO_PHASE_1)
        ORCA_DUAL_OPERATORS(SIM_EXPAND_DUAL_PHASE_1)
      }
    }
  }
}

void orca_run(Gbuffer gbuf, Mbuffer mbuf, Usz height, Usz width) {
  mbuffer_clear(mbuf, height, width);
  sim_phase_0(gbuf, mbuf, height, width);
  sim_phase_1(gbuf, mbuf, height, width);
}
