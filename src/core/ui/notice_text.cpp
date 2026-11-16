// The notice texts the core shows in the games' boxes (core/ui/notice.h): every string literal here is text for
// players (checked by the settings template check for both games).
#include "core/ui/notice.h"

namespace core::ui::notice {

const char *const kZoomFormat = "Zoom: %d%%";
// The pictures' (moved from Yongle's options_text.cpp). The numbers are their widths in Yongle's notice font (px,
// measured from the font): Yongle's box is 400 pixels wide for a text up to 320 pixels (with its side
// ornaments), 460 for a longer one. Errors are shown in red.
const char *const kCityPictureSaved = "City screenshot saved in the shots folder.";       // 279
const char *const kMinimapPictureSaved = "Minimap screenshot saved in the shots folder."; // 309 (still the 400 box)
const char *const kPictureNotSaved = "Screenshot could not be saved; see the log.";      // 291
const char *const kPictureBusy = "Still saving the last screenshot.";                    // 212
const char *const kPictureOnlyCity = "Screenshots work only in the city view.";           // 265
const char *const kPictureUnavailable = "The screenshots are not available now.";        // 261
const char *const kPictureNoMemory = "No memory for the screenshot; not saved.";         // 279
const char *const kPictureClosePanel = "Close the debug panel first.";                   // 186

} // namespace core::ui::notice
