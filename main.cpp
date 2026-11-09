/*
 * BBK - use a BlackBerry Q20 as a Bluetooth keyboard and mouse.
 *
 * The phone's own keyboard types on the PC, and the screen works as a
 * touchpad: dragging moves the pointer, tapping is the left button, and the
 * buttons underneath cover the right and middle buttons.  The hardware
 * trackpad under the screen sends arrow keys.
 *
 * The Bluetooth side is QNX's own HID device stack (btapi/bthid.h); see
 * bbkhid.h for the report layout.
 */
#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <QtCore/QElapsedTimer>
#include <QtCore/QSet>
#include <QtCore/QTime>
#include <cstdio>
#include <QtCore/QSignalMapper>
#include <QtCore/QTimer>
#include <QtGui/QApplication>
#include <QtGui/QDesktopWidget>
#include <QtGui/QMouseEvent>
#include <QtGui/QTouchEvent>
#include <QtGui/QLinearGradient>
#include <QtGui/QPainter>
#include <QtGui/QToolButton>
#include <QtGui/QVBoxLayout>
#include <QtGui/QHBoxLayout>
#include <QtGui/QWidget>

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <btapi/btdevice.h>
#include <btapi/bthid.h>

#include <QtCore/QAbstractEventDispatcher>

#include <screen/screen.h>
#include <bps/bps.h>
#include <bps/navigator.h>
#include <bps/screen.h>
#include <bps/event.h>

#include "bbkhid.h"

// How many touch pixels make one mouse pixel.
static const int kTouchScale = 2;

// The keys the Q20 keyboard cannot produce at all, laid out like
// BGShellBB10's key strip.
struct KeyDef { const char *label; int usage; int mods; };
static const KeyDef gKeys[] = {
	{ "Esc",  0x29, 0x00 }, { "Tab",  0x2b, 0x00 },
	{ "Home", 0x4a, 0x00 }, { "End",  0x4d, 0x00 },
	{ "PgUp", 0x4b, 0x00 }, { "PgDn", 0x4e, 0x00 },
	{ "Del",  0x4c, 0x00 },
};
static const KeyDef *bbkKeys()    { return gKeys; }
static const int     bbkKeyCount(){ return (int)(sizeof(gKeys) / sizeof(gKeys[0])); }

// ---------------------------------------------------------------- trackpad
// The Q20's trackpad does NOT arrive as Qt key events.  It shows up as a BPS
// SCREEN_EVENT_JOYSTICK with a displacement, so it has to be picked up from
// the platform event filter - exactly how BGShellBB10 drives its candidate
// strip.  Without this the trackpad is simply dead.
static BbkHid *gPadHid = 0;
static QAbstractEventDispatcher::EventFilter gPrevFilter = 0;

// One stroke picks one axis (BGShellBB10 does the same): a thumb never
// swipes perfectly straight, and deciding per sample made a sideways swipe
// jump up or down as soon as one sample leaned.  The lock is dropped after
// a 180 ms pause, which on a trackpad means the finger was lifted.
static int       gPadSwipeX = 0, gPadSwipeY = 0;
static int       gPadSwipeAxis = 0;      // 0 undecided, 1 across, 2 down
static long long gPadSwipeLast = 0;

// Tuning: travel needed for one arrow step, per axis.  Larger is calmer.
static const int kPadStepX   = 16;       // distance per left/right step
static const int kPadStepY   = 32;       // distance per up/down step
static const int kPadAxisLock = 12;      // travel before the axis is decided
static const int kPadGapMs    = 180;     // pause that ends the gesture
static const int kPadRepeatMs = 100;     // floor on how often one swipe fires
static long long gPadArrowLast = 0;

// The character the held Alt produced, kept so its release can be sent even
// if the release event no longer reports the translation.
static int gAltChar = 0;

// ------------------------------------------------------------ keep awake
// The trackpad and the touch screen ARE the mouse, so the phone must not go
// to sleep while BBK is open.  Two mechanisms are used together, because
// either one alone is not enough:
//
//  1. the display's idle timeout is pushed far out, so the screen does not
//     time out while BBK is in the foreground;
//  2. the window is marked SCREEN_IDLE_MODE_KEEP_AWAKE, which is what
//     actually stops a visible window from being counted as idle.
//
// The window handle comes from the events themselves, so it is the window
// the user is really interacting with.
static screen_context_t gAwakeCtx = 0;
static screen_window_t  gAwakeWin = 0;

static void keepScreenAwake()
{
	if (!gAwakeCtx)
		return;

	static int lastLog = 0;
	const bool wantLog = (++lastLog <= 4);

	// 1. Push the display's idle timeout out of the way.  The value is in
	//    seconds; an hour is far longer than any session.
	int dispCount = 0;
	if (screen_get_context_property_iv(gAwakeCtx,
	                                   SCREEN_PROPERTY_DISPLAY_COUNT,
	                                   &dispCount) == 0 && dispCount > 0) {
		void *disps = calloc(dispCount, sizeof(void *));
		if (disps) {
			// The API wants the buffer itself as void**: passing &disps
			// would hand it the address of the pointer variable, so it
			// would write dispCount handles into eight bytes of stack.
			if (screen_get_context_property_pv(gAwakeCtx,
			                                   SCREEN_PROPERTY_DISPLAYS,
			                                   (void **)disps) == 0) {
				const long long timeout = 3600;
				for (int i = 0; i < dispCount; ++i) {
					screen_display_t d = (screen_display_t)((void **)disps)[i];
					if (screen_set_display_property_llv(d,
					        SCREEN_PROPERTY_IDLE_TIMEOUT,
					        &timeout) != 0 && wantLog) {
						qDebug("BBK: keepawake: display idle timeout "
						       "failed: %s", strerror(errno));
					}
				}
			}
			free(disps);
		}
	}

	// 2. Mark the window as one that must stay awake.
	const int mode = SCREEN_IDLE_MODE_KEEP_AWAKE;
	if (gAwakeWin) {
		if (screen_set_window_property_iv(gAwakeWin,
		                                  SCREEN_PROPERTY_IDLE_MODE,
		                                  &mode) == 0) {
			// Property changes are delayed execution: they only take
			// effect at the next flush.
			screen_flush_context(gAwakeCtx, 0);
			if (wantLog)
				qDebug("BBK: keepawake: window marked KEEP_AWAKE");
			return;
		}
		gAwakeWin = 0;                 // stale handle, find it again
	}

	// No window handle yet: take every window the context owns.
	int count = 0;
	if (screen_get_context_property_iv(gAwakeCtx,
	                                   SCREEN_PROPERTY_WINDOW_COUNT,
	                                   &count) != 0 || count <= 0) {
		if (wantLog)
			qDebug("BBK: keepawake: no windows (count=%d)", count);
		return;
	}
	void *wins = calloc(count, sizeof(void *));
	if (!wins)
		return;
	if (screen_get_context_property_pv(gAwakeCtx, SCREEN_PROPERTY_WINDOWS,
	                                   (void **)wins) != 0) {
		if (wantLog)
			qDebug("BBK: keepawake: window list failed");
		free(wins);
		return;
	}
	int ok = 0;
	for (int i = 0; i < count; ++i) {
		screen_window_t win = (screen_window_t)((void **)wins)[i];
		if (screen_set_window_property_iv(win, SCREEN_PROPERTY_IDLE_MODE,
		                                  &mode) == 0) {
			if (!gAwakeWin)
				gAwakeWin = win;
			++ok;
		}
	}
	free(wins);
	if (wantLog)
		qDebug("BBK: keepawake: %d/%d window(s) marked KEEP_AWAKE", ok, count);
	if (ok > 0)
		screen_flush_context(gAwakeCtx, 0);
}

// ------------------------------------------------------------ multitouch
// Qt does not turn two-finger touches into QTouchEvents here, so the raw
// SCREEN_EVENT_MTOUCH_* stream is read instead.  A two-finger tap has to
// become the right button; there is no other way to ask for it.
//
// The same stream carries the pinch: with two fingers down, the distance
// between them is what changes, and that becomes the zoom.
struct MtFinger { int id; int x, y; };
static MtFinger  gMt[10];
static int       gMtCount = 0;
static int       gMtMax = 0;
static bool      gMtMoved = false;
static long long gMtStartMs = 0;

// Pinch state: the distance between the two fingers the last time it was
// measured, and how much zoom has been produced so far.
static int   gPinchLastDist = 0;
static bool  gPinching = false;
static int   gPinchAcc = 0;

static long long monoMs()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static bool bbkEventFilter(void *message)
{
	bps_event_t *event = static_cast<bps_event_t *>(message);
	if (event && bps_event_get_domain(event) == screen_get_domain()) {
		screen_event_t se = screen_event_get_event(event);
		int type = 0;
		screen_get_event_property_iv(se, SCREEN_PROPERTY_TYPE, &type);

		// Remember the context and the window these events belong to: the
		// window is what has to be marked KEEP_AWAKE so the phone does not
		// fall asleep while it is being used as a mouse.
		{
			screen_context_t ctx = screen_event_get_context(event);
			if (ctx && ctx != gAwakeCtx) {
				gAwakeCtx = ctx;
				gAwakeWin = 0;
				qDebug("BBK: keepawake: context %p", (void *)ctx);
			}
			void *win = 0;
			if (screen_get_event_property_pv(se, SCREEN_PROPERTY_WINDOW,
			                                 &win) == 0 && win) {
				gAwakeWin = (screen_window_t)win;
				keepScreenAwake();
			}
		}

		{
			static int typeLog = 0;
			if (typeLog < 40) {
				++typeLog;
				qDebug("BBK: screen event type=%d", type);
			}
		}

		if (type == SCREEN_EVENT_MTOUCH_TOUCH
		    || type == SCREEN_EVENT_MTOUCH_MOVE
		    || type == SCREEN_EVENT_MTOUCH_RELEASE) {
			int id = -1;
			int pos[2] = { 0, 0 };
			screen_get_event_property_iv(se, SCREEN_PROPERTY_TOUCH_ID, &id);
			screen_get_event_property_iv(se, SCREEN_PROPERTY_POSITION, pos);

			if (type == SCREEN_EVENT_MTOUCH_TOUCH) {
				if (gMtCount == 0) {
					gMtStartMs = monoMs();
					gMtMax = 0;
					gMtMoved = false;
				}
				if (gMtCount < 10) {
					gMt[gMtCount].id = id;
					gMt[gMtCount].x = pos[0];
					gMt[gMtCount].y = pos[1];
					++gMtCount;
				}
				if (gMtCount > gMtMax)
					gMtMax = gMtCount;
			} else if (type == SCREEN_EVENT_MTOUCH_MOVE) {
				for (int i = 0; i < gMtCount; ++i) {
					if (gMt[i].id != id)
						continue;
					const int dx = pos[0] - gMt[i].x;
					const int dy = pos[1] - gMt[i].y;
					if (dx > 15 || dx < -15 || dy > 15 || dy < -15)
						gMtMoved = true;
					gMt[i].x = pos[0];
					gMt[i].y = pos[1];
				}
				// Pinch: the distance between the two fingers is the
				// gesture.  Spreading them zooms in, pinching them
				// zooms out, which is what Ctrl+wheel does in most
				// programs.
				if (gMtCount == 2 && gPadHid) {
					const int dx = gMt[0].x - gMt[1].x;
					const int dy = gMt[0].y - gMt[1].y;
					const int dist = (int)sqrt((double)(dx * dx + dy * dy));
					if (!gPinching) {
						gPinching = true;
						gPinchLastDist = dist;
						gPinchAcc = 0;
					} else {
						const int delta = dist - gPinchLastDist;
						gPinchLastDist = dist;
						// One notch per 24 pixels of finger travel.
						gPinchAcc += delta;
						const int step = 24;
						int notches = gPinchAcc / step;
						if (notches != 0) {
							gPinchAcc -= notches * step;
							gPadHid->mouseZoom(notches);
							static int pinchLog = 0;
							if (pinchLog < 20) {
								++pinchLog;
								qDebug("BBK: pinch zoom %d notch(es)", notches);
							}
						}
					}
				}
			} else {                     // SCREEN_EVENT_MTOUCH_RELEASE
				for (int i = 0; i < gMtCount; ++i) {
					if (gMt[i].id == id) {
						gMt[i] = gMt[gMtCount - 1];
						--gMtCount;
						break;
					}
				}
				if (gMtCount < 2) {
					gPinching = false;
					gPinchAcc = 0;
				}
				if (gMtCount == 0) {
					const long long dur = monoMs() - gMtStartMs;
					qDebug("BBK: mt end max=%d moved=%d dur=%lld",
					       gMtMax, gMtMoved ? 1 : 0, dur);
					if (gMtMax == 2 && !gMtMoved && dur < 400 && gPadHid) {
						qDebug("BBK: two-finger tap -> right click");
						gPadHid->mouseButton(2, true);
						gPadHid->mouseButton(2, false);
					} else if (gMtMax == 3 && !gMtMoved && dur < 400 && gPadHid) {
						qDebug("BBK: three-finger tap -> middle click");
						gPadHid->mouseButton(3, true);
						gPadHid->mouseButton(3, false);
					}
				}
			}
		}

		if (type == SCREEN_EVENT_JOYSTICK) {
			int disp[2] = { 0, 0 };
			int buttons = 0;
			screen_get_event_property_iv(se, SCREEN_PROPERTY_DISPLACEMENT, disp);
			screen_get_event_property_iv(se, SCREEN_PROPERTY_BUTTONS, &buttons);

			static int logged = 0;
			if (logged < 40) {
				++logged;
				qDebug("BBK: trackpad disp=%d,%d buttons=%d",
				       disp[0], disp[1], buttons);
			}

			if (gPadHid) {
				// Arrows, not pointer movement: the pad is small and a
				// discrete step in the dominant direction is far easier
				// to aim than a relative cursor.  HID usages: 0x50 Left,
				// 0x51 Right, 0x52 Down, 0x53 Up.  Thresholds and axis
				// lock follow BGShellBB10's trackpad handling.
				if (disp[0] || disp[1]) {
					const long long nowMs = monoMs();
					if (nowMs - gPadSwipeLast > kPadGapMs) {
						gPadSwipeX = 0;
						gPadSwipeY = 0;
						gPadSwipeAxis = 0;
					}
					gPadSwipeLast = nowMs;

					// Every sample counts, however small: gating on a
					// single sample threw away slow swipes, which is
					// why one direction could go dead.
					gPadSwipeX += disp[0];
					gPadSwipeY += disp[1];

					if (gPadSwipeAxis == 0
					    && (qAbs(gPadSwipeX) >= kPadAxisLock
					        || qAbs(gPadSwipeY) >= kPadAxisLock))
						gPadSwipeAxis = (qAbs(gPadSwipeX) > qAbs(gPadSwipeY)) ? 1 : 2;

					int dx = 0, dy = 0;
					if (gPadSwipeAxis == 1 && qAbs(gPadSwipeX) >= kPadStepX) {
						dx = (gPadSwipeX > 0) ? 1 : -1;
						gPadSwipeX -= kPadStepX * dx;
					} else if (gPadSwipeAxis == 2 && qAbs(gPadSwipeY) >= kPadStepY) {
						dy = (gPadSwipeY > 0) ? 1 : -1;
						gPadSwipeY -= kPadStepY * dy;
					}

					// One arrow at most every kPadRepeatMs: a fast swipe
					// otherwise fires a dozen of them and overshoots.
					if ((dx != 0 || dy != 0)
					    && nowMs - gPadArrowLast >= kPadRepeatMs) {
						gPadArrowLast = nowMs;
						// HID usages: Right 0x4F, Left 0x50,
						// Down 0x51, Up 0x52.
						if (dx > 0)      gPadHid->tapKey(0x4f);
						else if (dx < 0) gPadHid->tapKey(0x50);
						if (dy > 0)      gPadHid->tapKey(0x51);
						else if (dy < 0) gPadHid->tapKey(0x52);
						static int keyLog = 0;
						if (keyLog < 40) {
							++keyLog;
							qDebug("BBK: pad arrow dx=%d dy=%d (acc %d,%d axis %d)",
							       dx, dy, gPadSwipeX, gPadSwipeY, gPadSwipeAxis);
						}
					}
				}
				if (buttons) {
					gPadHid->mouseButton(1, true);
					gPadHid->mouseButton(1, false);
				}
			}
		}
	}

	// The Back key never becomes a Qt key event.  On 10.3.1 the navigator
	// posts NAVIGATOR_SYSKEY_PRESS and waits for the app to say whether it
	// handled the key; answering "no" lets the system close the app, which
	// is exactly what happened before.  Claim it and use it as the right
	// mouse button, the way a laptop pad's second button works.
	if (event && bps_event_get_domain(event) == navigator_get_domain()) {
		const int code = bps_event_get_code(event);
		static int loggedNav = 0;
		if (loggedNav < 15) {
			++loggedNav;
			qDebug("BBK: navigator event %d", code);
		}
		if (code == NAVIGATOR_SYSKEY_PRESS) {
			const int key = navigator_event_get_syskey_key(event);
			const char *id = navigator_event_get_syskey_id(event);
			qDebug("BBK: syskey key=%d id=%s", key, id ? id : "?");
			if (key == NAVIGATOR_SYSKEY_BACK) {
				navigator_syskey_press_response(id, true);   // we handle it
				if (gPadHid) {
					gPadHid->mouseButton(2, true);
					gPadHid->mouseButton(2, false);
				}
			} else {
				navigator_syskey_press_response(id, false);  // leave it to the system
			}
		}
	}

	if (gPrevFilter)
		gPrevFilter(message);
	return false;
}

static const char *kDefaultPcAddr = "C4:3D:1A:EF:75:53";

// Ask the Bluetooth stack which devices are paired.  The PC is normally the
// only one, so its address can be filled in automatically instead of being
// typed on a phone keyboard.
// A Bluetooth class-of-device whose major class is "Computer".  That is the
// HID host - the PC - and it tells it apart from the earbuds and speakers
// that are usually connected at the same time.
static bool isComputer(const bt_remote_device_t *device)
{
	// Returns the major class itself, or -1 on error.
	return bt_rdev_get_device_class(device, BT_COD_MAJORDEVICECLASS) == 0x01;
}

// The peer to attach to: the device that is connected right now, ideally the
// PC.  The paired list alone is not good enough - after a reboot its first
// entry was an earbud, so the HID link never came up.
static QString peerAddr()
{
	bt_device_init(0);

	QString connBest, connAny, pairBest, pairAny;
	const QString pc = QString::fromLatin1(kDefaultPcAddr);

	for (int pass = 0; pass < 2; ++pass) {
		const int kind = (pass == 0) ? BT_DISCOVERY_CONNECTED
		                             : BT_DISCOVERY_PREKNOWN;
		int count = 0;
		bt_remote_device_t **arr = bt_disc_retrieve_devices(kind, &count);
		if (!arr)
			continue;
		for (int i = 0; i < count; ++i) {
			char addr[64] = { 0 };
			bool paired = false;
			if (bt_rdev_get_address(arr[i], addr) != 0)
				continue;
			bt_rdev_is_paired(arr[i], &paired);
			const QString a = QString::fromLatin1(addr);
			const bool good = isComputer(arr[i])
			    || a.compare(pc, Qt::CaseInsensitive) == 0;

			if (pass == 0) {
				qDebug("BBK: connected device %s paired=%d pc=%d",
				       addr, paired ? 1 : 0, good ? 1 : 0);
				if (connAny.isEmpty())
					connAny = a;
				if (good && connBest.isEmpty())
					connBest = a;
			} else if (paired) {
				if (pairAny.isEmpty())
					pairAny = a;
				if (good && pairBest.isEmpty())
					pairBest = a;
			}
		}
		bt_rdev_free_array(arr);
	}
	bt_device_deinit();

	QString out = !connBest.isEmpty() ? connBest
	            : !pairBest.isEmpty() ? pairBest
	            : !connAny.isEmpty()  ? connAny
	            : pairAny;
	qDebug("BBK: peer lookup -> %s", qPrintable(out));
	return out;
}

// A drag longer than this counts as movement, not as a tap.
static const int kTapSlop = 8;

static const QColor kBg     (0x0b, 0x0e, 0x11);
static const QColor kText   (0xe8, 0xee, 0xf4);
static const QColor kAccent (0x00, 0xd0, 0x60);

static const char *kButtonStyle =
    "QToolButton { background: #171e25; color: #e8eef4;"
    "  border: 1px solid #232d36; border-radius: 9px;"
    "  font-size: 8pt; padding: 0px; }"
    "QToolButton:pressed { background: #0a3d22;"
    "  border-color: #00d060; color: #ffffff; }";

// The touchpad.  Reports relative movement, which is what a mouse does.
class TouchPad : public QWidget
{
	Q_OBJECT

public:
	explicit TouchPad(QWidget *parent = 0)
	    : QWidget(parent), _lastX(0), _lastY(0), _moved(0), _down(false),
	      _scale(2)
	{
		setMinimumHeight(220);
		setMouseTracking(true);
		setFocusPolicy(Qt::StrongFocus);
		setAttribute(Qt::WA_OpaquePaintEvent, true);
		// BB10 sends one finger as a plain mouse drag and only the two-finger
		// gestures as touch events, so this costs the single-finger path
		// nothing and is what makes two-finger scrolling possible.
		setAttribute(Qt::WA_AcceptTouchEvents, true);
		_connected = false;
		_inScrollZone = false;
		_inRightZone = false;
		_dragActive = false;
		_lastTapValid = false;
		_lastTapX = 0;
		_lastTapY = 0;
		_scrollAccPx = 0;
		_panAccPx = 0;
		_touchFingers = 0;
		_touchMoved = 0;
		_touchLastX = 0;
		_touchLastY = 0;
		_touchScrollAccPx = 0;
	}

	// The border is the only status indicator: green when the PC is linked,
	// grey otherwise.
	void setConnected(bool on)
	{
		if (_connected == on)
			return;
		_connected = on;
		update();
	}

protected:
	void paintEvent(QPaintEvent *)
	{
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing, true);
		p.fillRect(rect(), kBg);

		const QRectF pad(1.5, 1.5, width() - 3, height() - 3);
		const int radius = 22;

		// A soft vertical gradient reads as a recessed surface rather than a
		// flat rectangle.
		QLinearGradient g(pad.topLeft(), pad.bottomLeft());
		g.setColorAt(0.0, QColor(0x1a, 0x22, 0x2a));
		g.setColorAt(1.0, QColor(0x12, 0x18, 0x1e));
		p.setPen(Qt::NoPen);
		p.setBrush(g);
		p.drawRoundedRect(pad, radius, radius);

		// Connected: a green edge plus a faint inner glow.  Otherwise a plain
		// dim edge.  This is the only status indicator.
		if (_connected) {
			p.setPen(QPen(QColor(0x00, 0xd0, 0x60, 60), 6));
			p.setBrush(Qt::NoBrush);
			p.drawRoundedRect(pad.adjusted(3, 3, -3, -3), radius - 3, radius - 3);
			p.setPen(QPen(kAccent, 2));
		} else {
			p.setPen(QPen(QColor(0x2a, 0x34, 0x3e), 1));
		}
		p.setBrush(Qt::NoBrush);
		p.drawRoundedRect(pad, radius, radius);

		// The scroll strip and its right-button corner, quietly marked.
		const qreal sx = width() * 3 / 4;
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(0x21, 0x2a, 0x33, 170));
		p.drawRoundedRect(QRectF(sx + 8, 14, width() - sx - 22, height() - 28),
		                  11, 11);
		p.setBrush(QColor(0x2e, 0x3c, 0x48, 220));
		p.drawRoundedRect(QRectF(sx + 8, height() * 3 / 4 + 8,
		                         width() - sx - 22, height() / 4 - 22), 11, 11);

		// Crosshair in the middle of the pointer area.
		const int cx = (int)(sx / 2);
		const int cy = height() / 2;
		p.setBrush(Qt::NoBrush);
		p.setPen(QPen(QColor(0x46, 0x58, 0x66), 1));
		p.drawLine(cx - 16, cy, cx + 16, cy);
		p.drawLine(cx, cy - 16, cx, cy + 16);
		p.drawEllipse(QPointF(cx, cy), 26, 26);
	}

	void mousePressEvent(QMouseEvent *e)
	{
		beginPress(e, false);
	}

	// Qt may or may not turn two quick taps into a double-click event on
	// this platform, so that path is accepted as well; whichever arrives
	// first wins, and both do the same thing.
	void mouseDoubleClickEvent(QMouseEvent *e)
	{
		beginPress(e, true);
	}

	void mouseMoveEvent(QMouseEvent *e)
	{
		if (!_down)
			return;

		const int dx = e->pos().x() - _lastX;
		const int dy = e->pos().y() - _lastY;
		_lastX = e->pos().x();
		_lastY = e->pos().y();
		_moved += qAbs(dx) + qAbs(dy);

		if (_dragActive) {
			// The left button is already down, so this is a drag-select.
			if (_moved > kTapSlop)
				emit moved(dx / _scale, dy / _scale);
		} else if (_moved <= kTapSlop) {
			return;
		} else if (_inScrollZone) {
			// A sideways drag in the scroll strip scrolls horizontally;
			// up and down scrolls the page.
			if (qAbs(dx) > qAbs(dy)) {
				_panAccPx += dx;
				const int step = 24;
				const int notches = _panAccPx / step;
				if (notches != 0) {
					_panAccPx -= notches * step;
					emit panned(notches);
				}
			} else {
				// Finger up scrolls the page up.
				_scrollAccPx += -dy;
				const int step = 18;
				const int notches = _scrollAccPx / step;
				if (notches != 0) {
					_scrollAccPx -= notches * step;
					emit scrolled(notches);
				}
			}
		} else {
			emit moved(dx / _scale, dy / _scale);
		}
		e->accept();
	}

	void mouseReleaseEvent(QMouseEvent *e)
	{
		// Second tap: either the second click of a double click, or the end
		// of a drag-select - either way the button that went down on the
		// press has to come up now.
		if (_dragActive) {
			emit dragEnded();
			_dragActive = false;
			_down = false;
			e->accept();
			return;
		}

		if (_down && _moved <= kTapSlop) {
			// Tap in the bottom-right corner is the right button; anywhere
			// else (including the scroll strip) is a plain click.
			if (_inRightZone) {
				emit rightClicked();
			} else {
				emit tapped();
				// Remember it, in case the next tap is the second half of
				// a double click that Qt never synthesizes.
				_lastTapX = e->pos().x();
				_lastTapY = e->pos().y();
				_lastTapTimer.start();
				_lastTapValid = true;
			}
		}
		_down = false;
		e->accept();
	}

	// Two fingers never arrive as mouse events on BB10 - only the synthesized
	// one-finger drag does - so this is where the laptop-style gestures live:
	// a two-finger tap is the right button, a two-finger slide scrolls.
	void touchEvent(QTouchEvent *e)
	{
		const QList<QTouchEvent::TouchPoint> pts = e->touchPoints();

		{
			static int tlog = 0;
			if (tlog < 20) {
				++tlog;
				qDebug("BBK: touch type=%d fingers=%d", (int)e->type(), pts.count());
			}
		}

		switch (e->type()) {
		case QEvent::TouchBegin:
			_touchFingers = pts.count();
			_touchMoved   = 0;
			_touchStart   = QTime::currentTime();
			_touchLastX   = averageX(pts);
			_touchLastY   = averageY(pts);
			_touchScrollAccPx = 0;
			break;

		case QEvent::TouchUpdate:
			if (pts.isEmpty())
				break;
			{
				const qreal x = averageX(pts);
				const qreal y = averageY(pts);
				const int dx = (int)(x - _touchLastX);
				const int dy = (int)(y - _touchLastY);
				_touchLastX = x;
				_touchLastY = y;
				_touchMoved += qAbs(dx) + qAbs(dy);
				if (_touchFingers == 2 && _touchMoved > kTapSlop) {
					// Same feel as the right-hand strip: finger up scrolls up.
					_touchScrollAccPx += -dy;
					const int step = 18;
					const int notches = _touchScrollAccPx / step;
					if (notches != 0) {
						_touchScrollAccPx -= notches * step;
						emit scrolled(notches);
					}
				}
			}
			break;

		case QEvent::TouchEnd:
			if (_touchFingers == 2 && _touchMoved <= kTapSlop
			    && _touchStart.msecsTo(QTime::currentTime()) < 400) {
				qDebug("BBK: two-finger tap -> right click");
				emit rightClicked();
			}
			_touchFingers = 0;
			break;

		default:
			break;
		}
		e->accept();
	}

	// Common start for a press and for Qt's double-click event.  A second
	// tap presses the left button down immediately: if the finger then
	// moves it drags a selection, and if it just lifts, that release is
	// the second click Windows needs for a double click.  The timing is
	// measured here rather than trusted to Qt, which does not turn quick
	// taps into QEvent::MouseButtonDblClick on this device.
	void beginPress(QMouseEvent *e, bool qtDouble)
	{
		const bool second = qtDouble
		    || (_lastTapValid
		        && _lastTapTimer.elapsed() <= QApplication::doubleClickInterval()
		        && qAbs(e->pos().x() - _lastTapX) <= 24
		        && qAbs(e->pos().y() - _lastTapY) <= 24);
		_lastTapValid = false;

		_lastX = e->pos().x();
		_lastY = e->pos().y();
		_moved = 0;
		_down  = true;
		// The right-hand strip scrolls and its bottom corner is the right
		// button, the way a laptop pad works.
		_inScrollZone = (e->pos().x() > width() * 3 / 4);
		_inRightZone  = _inScrollZone && (e->pos().y() > height() * 3 / 4);
		_scrollAccPx  = 0;
		setFocus();

		_dragActive = second && !_inScrollZone;
		if (_dragActive) {
			qDebug("BBK: double tap -> left button held");
			emit dragBegan();
		}
		e->accept();
	}

signals:
	void moved(int dx, int dy);
	void tapped();
	void scrolled(int notches);
	void panned(int notches);
	void rightClicked();
	void middleClicked();
	void dragBegan();
	void dragEnded();

private:
	static qreal averageX(const QList<QTouchEvent::TouchPoint> &pts)
	{
		if (pts.isEmpty())
			return 0;
		qreal sum = 0;
		for (int i = 0; i < pts.count(); ++i)
			sum += pts.at(i).pos().x();
		return sum / pts.count();
	}

	static qreal averageY(const QList<QTouchEvent::TouchPoint> &pts)
	{
		if (pts.isEmpty())
			return 0;
		qreal sum = 0;
		for (int i = 0; i < pts.count(); ++i)
			sum += pts.at(i).pos().y();
		return sum / pts.count();
	}

	int  _lastX, _lastY, _moved;
	bool _down;
	bool _connected;
	int  _scale;
	bool  _inScrollZone;
	bool  _inRightZone;
	bool  _dragActive;
	bool  _lastTapValid;
	int   _lastTapX, _lastTapY;
	QElapsedTimer _lastTapTimer;
	int   _scrollAccPx;
	int   _panAccPx;
	int   _touchFingers;
	int   _touchMoved;
	qreal _touchLastX, _touchLastY;
	int   _touchScrollAccPx;
	QTime _touchStart;
};

// The whole window.  It owns the HID device and forwards every key to it,
// so nothing the phone's keyboard does has a meaning of its own.
class BbkWindow : public QWidget
{
	Q_OBJECT

public:
	explicit BbkWindow(QWidget *parent = 0)
	    : QWidget(parent)
	{
		setWindowTitle(QString::fromUtf8("BBK"));
		_keyTaps = new QSignalMapper(this);
		_rightShift = false;
		_symDown = false;
		setFocusPolicy(Qt::StrongFocus);
		setAutoFillBackground(true);
		QPalette pal = palette();
		pal.setColor(QPalette::Window, kBg);
		setPalette(pal);

		QVBoxLayout *root = new QVBoxLayout(this);
		root->setContentsMargins(14, 14, 14, 12);
		root->setSpacing(10);

		_pad = new TouchPad(this);
		root->addWidget(_pad, 1);

		// The keys the Q20 keyboard cannot produce, in one compact strip.
		QHBoxLayout *keys = new QHBoxLayout;
		keys->setSpacing(5);
		const KeyDef *kKeys = bbkKeys();
		const int kCount = bbkKeyCount();
		for (int i = 0; i < kCount; ++i) {
			QToolButton *b = new QToolButton(this);
			// The strip labels are UTF-8; fromLatin1 turned those
			// bytes into mojibake.
			b->setText(QString::fromUtf8(kKeys[i].label));
			b->setMinimumHeight(46);
			b->setMinimumWidth(40);
			b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
			b->setFocusPolicy(Qt::NoFocus);
			b->setStyleSheet(kButtonStyle);
			keys->addWidget(b, 1);
			QObject::connect(b, SIGNAL(clicked()), _keyTaps, SLOT(map()));
			_keyTaps->setMapping(b, i);
		}
		root->addLayout(keys);
		connect(_keyTaps, SIGNAL(mapped(int)), this, SLOT(onShortcut(int)));

		// No address field and no connect button: the address is looked up
		// from the paired list and the link is kept up automatically.
		qApp->installEventFilter(this);

		// The trackpad speaks BPS, not Qt: hook the platform filter.
		gPadHid = &_hid;
		if (QAbstractEventDispatcher::instance()) {
			gPrevFilter = QAbstractEventDispatcher::instance()
			                  ->setEventFilter(bbkEventFilter);
		}

		connect(_pad, SIGNAL(moved(int,int)), &_hid, SLOT(mouseMove(int,int)));
		connect(_pad, SIGNAL(tapped()), this, SLOT(onTap()));
		connect(_pad, SIGNAL(scrolled(int)), this, SLOT(onScroll(int)));
		connect(_pad, SIGNAL(panned(int)), this, SLOT(onPan(int)));
		connect(_pad, SIGNAL(rightClicked()), this, SLOT(onRightClick()));
		connect(_pad, SIGNAL(middleClicked()), this, SLOT(onMiddleClick()));
		connect(_pad, SIGNAL(dragBegan()), this, SLOT(onDragBegan()));
		connect(_pad, SIGNAL(dragEnded()), this, SLOT(onDragEnded()));
		connect(&_hid, SIGNAL(peerConnected(QString)), this, SLOT(onPeer(QString)));
		connect(&_hid, SIGNAL(peerDisconnected()), this, SLOT(onPeerGone()));

		QString error;
		if (!_hid.start(&error)) {
			qDebug("BBK: start failed: %s", qPrintable(error));
		} else {
			onConnect();
		}

		// The link drops when the PC sleeps, when Windows re-enumerates it or
		// when the phone dozes.  Re-open it on a timer so the user never has
		// to touch anything.
		_keepAlive = new QTimer(this);
		_keepAlive->setInterval(5000);
		connect(_keepAlive, SIGNAL(timeout()), this, SLOT(onConnect()));
		_keepAlive->start();

		// Keep the display awake: the trackpad and the touch screen are
		// the mouse, so a sleeping screen makes BBK useless.  The timer
		// re-applies it in case the window was replaced.
		_awake = new QTimer(this);
		_awake->setInterval(10000);
		connect(_awake, SIGNAL(timeout()), this, SLOT(onKeepAwake()));
		_awake->start();
	}

protected:
	// Every key goes to the PC once the HID link is up, whatever has focus.
	//
	// The right-hand Shift on a BlackBerry keyboard is where a PC has Ctrl,
	// so it is held as Ctrl: the modifier is folded into every other key
	// while it is down.  (Both Shifts arrive as Qt::Key_Shift; the QNX keycode
	// is what tells them apart.)
	bool eventFilter(QObject *watched, QEvent *e)
	{
		if (_hid.isConnected()
		    && (e->type() == QEvent::KeyPress || e->type() == QEvent::KeyRelease)) {
			QKeyEvent *k = static_cast<QKeyEvent *>(e);
			const bool down = (e->type() == QEvent::KeyPress);

			// The special keys on this keyboard (SYM, the right Shift, the
			// call keys) have no obvious Qt equivalent; log every distinct
			// key code once so they can be mapped exactly.
			{
				static QSet<int> seen;
				if (down && !seen.contains((int)k->nativeVirtualKey())) {
					seen.insert((int)k->nativeVirtualKey());
					qDebug("BBK: KEYCODE key=0x%x nv=0x%x ns=0x%x mods=0x%x",
					       k->key(), (unsigned)k->nativeVirtualKey(),
					       (unsigned)k->nativeScanCode(), (unsigned)k->modifiers());
				}
			}

			// The right-hand Shift is where a PC has Ctrl.
			if (isRightShift(k)) {
				qDebug("BBK: RIGHT-SHIFT %s", down ? "down" : "up");
				_rightShift = down;
				// Still tell the HID side: Ctrl has to go down and come
				// back up, or it stays latched on the PC after a chord.
				_hid.keyEvent(Qt::Key_Control, Qt::ControlModifier, down);
				return true;          // consumed: it is Ctrl, not Shift
			}

			// SYM has no Qt equivalent (it arrives as Key_unknown, QNX
			// keycode 0xf0d3) and is where a PC has the Windows key.
			if (isSymKey(k)) {
				_symDown = down;
				_hid.keyEvent(Qt::Key_Meta, Qt::MetaModifier, down);
				return true;
			}

			// The BlackBerry menu key sits beside the trackpad, so it is the
			// left mouse button: press holds it down, release lets go, so
			// it can also drag.
			if (isMenuKey(k)) {
				qDebug("BBK: menu key %s -> left button", down ? "down" : "up");
				_hid.mouseButton(1, down);
				return true;
			}

			// The volume rocker and the media keys go to the PC's speaker.
			// The stack's HID descriptor has an "Enhanced Keys" report for
			// exactly these.
			{
				const int media = mediaUsage(k);
				if (media) {
					if (down)
						_hid.tapMediaKey(media);
					return true;
				}
			}

			// Alt is the phone's symbol shifter: Alt+E types the "2" printed
			// on the key.  Qt still reports key=E with AltModifier and the
			// character only shows up in nativeVirtualKey, so send that
			// character - carrying Alt through would make the PC read a
			// shortcut instead of a digit.
			if (!_rightShift && !_symDown) {
				if (gAltChar && !down) {
					_hid.typeChar(gAltChar, false);
					gAltChar = 0;
					return true;
				}
				int ch = 0;
				if (down && altChar(k, &ch)) {
					gAltChar = ch;
					_hid.typeChar(ch, true);
					return true;
				}
			}

			// Right Shift is Ctrl and NOTHING else.  The original modifiers
			// must be dropped: this keyboard latches Alt, so a plain C can
			// arrive as Alt+C, and OR-ing Ctrl on made it Ctrl+Alt+C, which
			// the PC does not treat as a copy.  BGShellBB10 does the same -
			// it builds the Ctrl chord itself and ignores the event's mods.
			// The D-pad drives the pointer, which makes fine positioning
			// possible without covering the screen with a finger.  Holding a
			// direction repeats (Qt auto-repeats), so it also works as a
			// slow continuous move.
			if (!_rightShift && !_symDown) {
				const int stepPx = 8;
				switch (k->key()) {
				case Qt::Key_Left:  if (down) _hid.mouseMove(-stepPx, 0); return true;
				case Qt::Key_Right: if (down) _hid.mouseMove( stepPx, 0); return true;
				case Qt::Key_Up:    if (down) _hid.mouseMove(0, -stepPx); return true;
				case Qt::Key_Down:  if (down) _hid.mouseMove(0,  stepPx); return true;
				}
			}

			int mods;
			if (_rightShift) {
				mods = Qt::ControlModifier;
			} else {
				mods = k->modifiers();
				if (_symDown)
					mods |= Qt::MetaModifier;
			}
			{
				static int sentLog = 0;
				if (sentLog < 20 && down) {
					++sentLog;
					qDebug("BBK: SEND key=0x%x mods=0x%x rs=%d sym=%d",
					       k->key(), (unsigned)mods,
					       _rightShift ? 1 : 0, _symDown ? 1 : 0);
				}
			}
			_hid.keyEvent(k->key(), mods, down);
			return true;
		}
		return QWidget::eventFilter(watched, e);
	}

	// Codes measured on the Q20 by logging every key press:
	//   right Shift  nv=0xf0e2  ns=0xe5   (Qt::Key_Shift)
	//   SYM          nv=0xf0d3  ns=0x71   (Qt::Key_unknown)
	static bool isRightShift(QKeyEvent *event)
	{
		const int nv = event->nativeVirtualKey();
		const int ns = event->nativeScanCode();
		return nv == 0xf0e2 || ns == 0xe5;
	}

	static bool isSymKey(QKeyEvent *event)
	{
		const int nv = event->nativeVirtualKey();
		const int ns = event->nativeScanCode();
		return nv == 0xf0d3 || ns == 0x71;
	}

	// The BlackBerry key beside the trackpad arrives as Qt::Key_Menu with
	// QNX keycode 0xf067.
	static bool isMenuKey(QKeyEvent *event)
	{
		return event->key() == Qt::Key_Menu
		    || event->nativeVirtualKey() == 0xf067;
	}

	// True when Alt turned this key into a different character - the
	// alternate symbol printed on the phone's key.  The character rides in
	// nativeVirtualKey; if it is just the key's own letter, Alt is being
	// used as a real modifier and the event must pass through untouched.
	static bool altChar(QKeyEvent *event, int *ch)
	{
		if (!(event->modifiers() & Qt::AltModifier))
			return false;
		if (event->key() == Qt::Key_Space)
			return false;               // Alt+Space is Tab
		const int nv = event->nativeVirtualKey();
		if (nv < 0x20 || nv > 0x7e)
			return false;
		const int qt = event->key();
		int plain = qt;
		if (qt >= Qt::Key_A && qt <= Qt::Key_Z)
			plain = qt - Qt::Key_A + 'a';
		else if (qt >= Qt::Key_0 && qt <= Qt::Key_9)
			plain = qt - Qt::Key_0 + '0';
		if (nv == plain)
			return false;
		*ch = nv;
		return true;
	}

	// The consumer-page usage for a volume or media key, or 0.  The phone
	// reports these either as Qt's own media keys or as the QNX consumer
	// key codes; both are accepted.
	//
	// Bits of the stack's "Enhanced Keys" report:
	//   Mute 0x01, VolumeUp 0x02, VolumeDown 0x04, Play/Pause 0x08,
	//   Stop 0x10, Next 0x20, Previous 0x40, Rewind 0x80.
	static int mediaUsage(QKeyEvent *event)
	{
		switch (event->key()) {
		case Qt::Key_VolumeUp:        return 0x02;
		case Qt::Key_VolumeDown:      return 0x04;
		case Qt::Key_VolumeMute:      return 0x01;
		case Qt::Key_MediaTogglePlayPause:
		case Qt::Key_MediaPlay:       return 0x08;
		case Qt::Key_MediaStop:       return 0x10;
		case Qt::Key_MediaNext:       return 0x20;
		case Qt::Key_MediaPrevious:   return 0x40;
		default: break;
		}

		// QNX consumer key codes: KEYCODE_CONSUMER_KEYS + offset.
		switch (event->nativeVirtualKey()) {
		case 0xf21d: return 0x02;      // volume up
		case 0xf21e: return 0x04;      // volume down
		case 0xf21c: return 0x01;      // mute
		case 0xf21a: return 0x08;      // play/pause
		case 0xf217: return 0x10;      // stop
		case 0xf215: return 0x20;      // next
		case 0xf216: return 0x40;      // previous
		default: break;
		}
		return 0;
	}

	void keyPressEvent(QKeyEvent *e)
	{
		_hid.keyEvent(e->key(), e->modifiers(), true);
		e->accept();
	}

	void keyReleaseEvent(QKeyEvent *e)
	{
		_hid.keyEvent(e->key(), e->modifiers(), false);
		e->accept();
	}

private slots:
	void onKeepAwake()
	{
		keepScreenAwake();
	}

	void onPeer(const QString &peer)
	{
		// No text on screen: the pad's border colour is the whole status.
		_pad->setConnected(true);
		setWindowTitle(QString::fromUtf8("BBK - ") + peer);
	}

	void onPeerGone()
	{
		_pad->setConnected(false);
		setWindowTitle(QString::fromUtf8("BBK"));
	}

	// Open the HID channel to the PC.  This is the step that makes Windows
	// install a HID keyboard/mouse for this phone.  Called at startup and
	// again every few seconds so a dropped link repairs itself.
	// One of the key-strip buttons: send that key (and modifier) to the PC.
	void onShortcut(int index)
	{
		if (index < 0 || index >= bbkKeyCount())
			return;
		_hid.tapKey(bbkKeys()[index].usage, bbkKeys()[index].mods);
	}

	void onConnect()
	{
		if (_hid.isConnected()) {
			// The stack never tells us that the peer went away, so the
			// link is checked by sending something.  Without this the app
			// keeps claiming to be connected after the PC slept.
			if (_hid.probe()) {
				_pad->setConnected(true);
				return;
			}
			qDebug("BBK: link lost, reconnecting");
			_hid.forceDisconnect();
			_pad->setConnected(false);
		}

		QString addr = _addr;
		if (addr.isEmpty()) {
			addr = peerAddr();                   // ask the stack
			if (addr.isEmpty())
				addr = QString::fromLatin1(kDefaultPcAddr);
			_addr = addr;
		}
		if (addr.isEmpty())
			return;

		QString error;
		const bool ok = _hid.connectTo(addr, &error);
		_pad->setConnected(ok);
		if (!ok) {
			// Pick afresh next tick: the wrong device may have been
			// chosen, or the PC may have gone out of range.
			_addr.clear();
		}
	}

	void onTap()
	{
		_hid.mouseButton(1, true);
		_hid.mouseButton(1, false);
	}

	// Two-finger vertical swipe: scroll the PC.
	void onScroll(int notches)
	{
		_hid.mouseWheel(-notches);      // finger up => page up
	}

	// Horizontal swipe: scroll sideways.
	void onPan(int notches)
	{
		_hid.mousePan(notches);
	}

	// Three-finger tap: the middle mouse button.
	void onMiddleClick()
	{
		_hid.mouseButton(3, true);
		_hid.mouseButton(3, false);
	}

	// Double-tap-then-drag: hold the left button down while the finger moves,
	// which is what selects text on the PC.
	void onDragBegan() { _hid.mouseButton(1, true); }
	void onDragEnded() { _hid.mouseButton(1, false); }

	// Tap in the pad's bottom-right corner.
	void onRightClick()
	{
		_hid.mouseButton(2, true);
		_hid.mouseButton(2, false);
	}

	void onLeft()    { _hid.mouseButton(1, true);  _hid.mouseButton(1, false); }
	void onRight()   { _hid.mouseButton(2, true);  _hid.mouseButton(2, false); }
	void onWheelUp() { _hid.mouseWheel(1); }
	void onWheelDown() { _hid.mouseWheel(-1); }

private:
	TouchPad    *_pad;
	BbkHid       _hid;
	QTimer      *_keepAlive;
	QTimer      *_awake;
	QString      _addr;
	QSignalMapper *_keyTaps;
	bool         _rightShift;
	bool         _symDown;
};

// --- hidtest callbacks -----------------------------------------------------
static void hdOnConnected(const char *addr)
{
	printf("hidtest: [CB] HOST CONNECTED: %s\n", addr ? addr : "?");
	fflush(stdout);
}
static void hdOnDisconnected(int reason)
{
	printf("hidtest: [CB] host disconnected (reason %d)\n", reason);
	fflush(stdout);
}
static void hdOnReportSet(const bt_hid_report_t *report)
{
	Q_UNUSED(report);
	bt_hid_set_report_res(BT_HID_RESULT_SUCCESS);
}

int main(int argc, char **argv)
{
	// "--list" prints the paired devices, so the HID connection can target
	// one without the address being typed in by hand.
	if (argc > 1 && QString::fromLatin1(argv[1]) == QLatin1String("--list")) {
		QCoreApplication app(argc, argv);
		bt_device_init(0);
		int count = 0;
		bt_remote_device_t **arr =
		    bt_disc_retrieve_devices(BT_DISCOVERY_PREKNOWN, &count);
		printf("list: %d paired device(s)\n", count);
		if (arr) {
			for (int i = 0; i < count; ++i) {
				char addr[64] = { 0 };
				char name[128] = { 0 };
				bt_rdev_get_address(arr[i], addr);
				bt_rdev_get_remote_name(arr[i], name, sizeof(name));
				bool paired = false;
				bt_rdev_is_paired(arr[i], &paired);
				printf("  [%d] %s  %s  paired=%d\n", i, addr, name, paired ? 1 : 0);
			}
			bt_rdev_free_array(arr);
		}
		bt_device_deinit();
		return 0;
	}

	// "--hidtest <pc-mac>" tries the CLASSIC HID device role and actively
	// connects to the given host.  Windows only ever saw AVRCP from this
	// phone, so the classic HID service has to be brought up and connected
	// from this side rather than waited for.
	if (argc > 2 && QString::fromLatin1(argv[1]) == QLatin1String("--hidtest")) {
		QCoreApplication app(argc, argv);
		const QByteArray mac = QByteArray(argv[2]);

		bt_hid_callbacks_t cb;
		memset(&cb, 0, sizeof(cb));
		cb.connected        = hdOnConnected;
		cb.disconnected     = hdOnDisconnected;
		cb.report_set       = hdOnReportSet;
		puts("hidtest: bt_hid_init..."); fflush(stdout);
		if (bt_hid_init(&cb) != 0) {
			printf("hidtest: init failed: %s\n", strerror(errno));
			return 1;
		}
		puts("hidtest: bt_hid_register..."); fflush(stdout);
		if (bt_hid_register() != 0) {
			printf("hidtest: register failed: %s\n", strerror(errno));
			bt_hid_deinit();
			return 1;
		}
		printf("hidtest: registered, connecting to %s ...\n", mac.constData());
		fflush(stdout);
		if (bt_hid_connect(mac.constData()) != 0) {
			printf("hidtest: connect failed: %s\n", strerror(errno));
		} else {
			puts("hidtest: CONNECTED");
			// Send a few 'A' key reports so the PC has something to show.
			for (int i = 0; i < 3; ++i) {
				bt_hid_report_t rep;
				uint8_t down[9] = { 0x01, 0x00, 0x04, 0,0,0,0,0,0 };
				rep.reportType = BT_HID_REPORT_INPUT;
				rep.dataLen = sizeof(down);
				rep.data = down;
				printf("hidtest: send key down -> %d\n", bt_hid_send_interrupt(&rep));
				usleep(60000);
				uint8_t up[9] = { 0x01, 0x00, 0x00, 0,0,0,0,0,0 };
				rep.data = up;
				printf("hidtest: send key up   -> %d\n", bt_hid_send_interrupt(&rep));
				usleep(60000);
			}
			fflush(stdout);
		}
		bt_hid_disconnect();
		bt_hid_deregister();
		bt_hid_deinit();
		puts("hidtest: done");
		return 0;
	}

	// "--validate" runs the GATT service registration and reports what the
	// stack thinks of it, without needing a window - handy over ssh.
	if (argc > 1 && QString::fromLatin1(argv[1]) == QLatin1String("--validate")) {
		QCoreApplication app(argc, argv);
		BbkHid hid;
		QString error;
		const bool ok = hid.start(&error);
		printf("validate: %s (%s)\n", ok ? "OK" : "FAILED",
		       qPrintable(error));
		fflush(stdout);
		return ok ? 0 : 1;
	}

	qDebug("BBK: main entered");
	QApplication app(argc, argv);
	qDebug("BBK: QApplication up");

	BbkWindow w;
	w.resize(QApplication::desktop()->screenGeometry(0).size());
	w.show();
	w.setFocus();

	return app.exec();
}

#include "main.moc"
