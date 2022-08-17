#ifndef SPRITE
#define SPRITE(name, sheetName, x, y, w, h)
#endif
#ifndef SHEET
#define SHEET(sheetName, filepath)
#endif
SHEET(Art, "data/sheet.png")

SPRITE(BorderBox, Art, 0, 0, 12, 12)
SPRITE(Box, Art, 12, 0, 12, 12)
SPRITE(Pixel, Art, 15, 4, 1, 1)
