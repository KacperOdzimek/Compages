/*
----------------------------------------------------------------
Contents:
This file builds all compages libraries, to avoid building one after another.

----------------------------------------------------------------
Code info:
- COMPAGES_IMPL macro to build
*/

#ifdef COMPAGES_IMPL

#define COMPAGES_SHAPES_RENDERING_IMPL
#include "compages/rendering/shapes_rendering.h"

#define COMPAGES_ARBOR_RENDERING_IMPL
#include "compages/rendering/arbor_rendering.h"

#define COMPAGES_FONT_IMPL
#include "compages/resources/font.h"

#define COMPAGES_SYNCHRONISED_WINDOW_IMPL
#include "compages/utility/synchronised_window.h"

#else

#include "compages/rendering/shapes_rendering.h"
#include "compages/rendering/arbor_rendering.h"
#include "compages/resources/font.h"
#include "compages/utility/synchronised_window.h"

#endif
