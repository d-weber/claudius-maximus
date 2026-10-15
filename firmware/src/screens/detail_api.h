#pragma once
// Extra session-detail API (owner: detail-feed). Core API lives in screens.h.
// Session id shown by the detail overlay, or "" when it is closed (e.g. alerts can skip
// a "needs you" card for the session the user is already looking at).
const char *detail_sid();
