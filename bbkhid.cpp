/*
 * See bbkhid.h.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <btapi/bthid.h>

#include <QtCore/QDebug>

#include "bbkhid.h"

static BbkHid *g_hid = 0;

// ---------------------------------------------------------------- HID callbacks
// The stack calls these from its own thread with no QObject context, so the
// instance is kept in a file static.
static void hidOnConnected(const char *addr)
{
	qDebug("BBK: host connected: %s", addr ? addr : "");
	if (g_hid)
		g_hid->notifyConnected(QString::fromLatin1(addr ? addr : ""));
}

static void hidOnDisconnected(int reason)
{
	qDebug("BBK: host disconnected (reason %d)", reason);
	if (g_hid)
		g_hid->notifyDisconnected();
}

static void hidOnReportSet(const bt_hid_report_t *report)
{
	Q_UNUSED(report);
	// The host sets LEDs (caps lock etc).  Acknowledge; the phone has no
	// LEDs to mirror.
	bt_hid_set_report_res(BT_HID_RESULT_SUCCESS);
}

// ---------------------------------------------------------------- key mapping
static int qtKeyToHid(int qtKey)
{
	switch (qtKey) {
	// A letter maps to one usage; the Shift bit in the report decides
	// whether the PC shows it as upper or lower case.  The phone always
	// reports the upper-case Qt code and puts the real case in the
	// modifiers, so the usage is the same either way.
	case Qt::Key_A: case Qt::Key_B: case Qt::Key_C: case Qt::Key_D:
	case Qt::Key_E: case Qt::Key_F: case Qt::Key_G: case Qt::Key_H:
	case Qt::Key_I: case Qt::Key_J: case Qt::Key_K: case Qt::Key_L:
	case Qt::Key_M: case Qt::Key_N: case Qt::Key_O: case Qt::Key_P:
	case Qt::Key_Q: case Qt::Key_R: case Qt::Key_S: case Qt::Key_T:
	case Qt::Key_U: case Qt::Key_V: case Qt::Key_W: case Qt::Key_X:
	case Qt::Key_Y: case Qt::Key_Z:
		return 0x04 + (qtKey - Qt::Key_A);
	case Qt::Key_1: return 0x1e;
	case Qt::Key_2: return 0x1f;
	case Qt::Key_3: return 0x20;
	case Qt::Key_4: return 0x21;
	case Qt::Key_5: return 0x22;
	case Qt::Key_6: return 0x23;
	case Qt::Key_7: return 0x24;
	case Qt::Key_8: return 0x25;
	case Qt::Key_9: return 0x26;
	case Qt::Key_0: return 0x27;
	// Both Enter flavours: the Q20's Enter arrives as Key_Return (the main
	// one), and a numeric keypad would send Key_Enter.  Only Key_Enter was
	// mapped, so Enter did nothing at all.
	case Qt::Key_Return:
	case Qt::Key_Enter:        return 0x28;
	case Qt::Key_Escape:       return 0x29;
	case Qt::Key_Backspace:    return 0x2a;
	case Qt::Key_Tab:          return 0x2b;
	case Qt::Key_Space:        return 0x2c;
	case Qt::Key_Minus:        return 0x2d;
	case Qt::Key_Equal:        return 0x2e;
	case Qt::Key_BracketLeft:  return 0x2f;
	case Qt::Key_BracketRight: return 0x30;
	case Qt::Key_Backslash:    return 0x31;
	case Qt::Key_Semicolon:    return 0x33;
	case Qt::Key_Apostrophe:   return 0x34;
	case Qt::Key_QuoteLeft:    return 0x35;
	case Qt::Key_Comma:        return 0x36;
	case Qt::Key_Period:       return 0x37;
	case Qt::Key_Slash:        return 0x38;
	case Qt::Key_CapsLock:     return 0x39;
	case Qt::Key_F1:  return 0x3a;
	case Qt::Key_F2:  return 0x3b;
	case Qt::Key_F3:  return 0x3c;
	case Qt::Key_F4:  return 0x3d;
	case Qt::Key_F5:  return 0x3e;
	case Qt::Key_F6:  return 0x3f;
	case Qt::Key_F7:  return 0x40;
	case Qt::Key_F8:  return 0x41;
	case Qt::Key_F9:  return 0x42;
	case Qt::Key_F10: return 0x43;
	case Qt::Key_F11: return 0x44;
	case Qt::Key_F12: return 0x45;
	// Usage IDs from the HID keyboard page: Insert 0x49, Home 0x4A,
	// PageUp 0x4B, Delete 0x4C, End 0x4D, PageDown 0x4E, Right 0x4F,
	// Left 0x50, Down 0x51, Up 0x52.
	case Qt::Key_Insert:    return 0x49;
	case Qt::Key_Home:      return 0x4a;
	case Qt::Key_PageUp:    return 0x4b;
	case Qt::Key_Delete:    return 0x4c;
	case Qt::Key_End:       return 0x4d;
	case Qt::Key_PageDown:  return 0x4e;
	case Qt::Key_Right:     return 0x4f;
	case Qt::Key_Left:      return 0x50;
	case Qt::Key_Down:      return 0x51;
	case Qt::Key_Up:        return 0x52;
	default:
		return 0;
	}
}

static void qtModifiersToHid(int mods, unsigned char *out)
{
	unsigned char m = 0;
	if (mods & Qt::ControlModifier) m |= 0x01;
	if (mods & Qt::ShiftModifier)   m |= 0x02;
	if (mods & Qt::AltModifier)     m |= 0x04;
	if (mods & Qt::MetaModifier)    m |= 0x08;
	*out = m;
}

// HID usage for a printable ASCII character, plus the Shift it needs.
// Returns 0 for anything that is not on a US keyboard.
static int asciiToHid(int ch, unsigned char *mods)
{
	*mods = 0;
	if (ch >= 'a' && ch <= 'z') return 0x04 + (ch - 'a');
	if (ch >= 'A' && ch <= 'Z') { *mods = 0x02; return 0x04 + (ch - 'A'); }
	if (ch >= '1' && ch <= '9') return 0x1e + (ch - '1');
	if (ch == '0') return 0x27;
	switch (ch) {
	case ' ':  return 0x2c;
	case '-':  return 0x2d;
	case '=':  return 0x2e;
	case '[':  return 0x2f;
	case ']':  return 0x30;
	case '\\': return 0x31;
	case ';':  return 0x33;
	case '\'': return 0x34;
	case '`':  return 0x35;
	case ',':  return 0x36;
	case '.':  return 0x37;
	case '/':  return 0x38;
	// Symbols that live on the shifted number/punctuation keys.
	case '!': *mods = 0x02; return 0x1e;
	case '@': *mods = 0x02; return 0x1f;
	case '#': *mods = 0x02; return 0x20;
	case '$': *mods = 0x02; return 0x21;
	case '%': *mods = 0x02; return 0x22;
	case '^': *mods = 0x02; return 0x23;
	case '&': *mods = 0x02; return 0x24;
	case '*': *mods = 0x02; return 0x25;
	case '(': *mods = 0x02; return 0x26;
	case ')': *mods = 0x02; return 0x27;
	case '_': *mods = 0x02; return 0x2d;
	case '+': *mods = 0x02; return 0x2e;
	case '{': *mods = 0x02; return 0x2f;
	case '}': *mods = 0x02; return 0x30;
	case '|': *mods = 0x02; return 0x31;
	case ':': *mods = 0x02; return 0x33;
	case '"': *mods = 0x02; return 0x34;
	case '~': *mods = 0x02; return 0x35;
	case '<': *mods = 0x02; return 0x36;
	case '>': *mods = 0x02; return 0x37;
	case '?': *mods = 0x02; return 0x38;
	}
	return 0;
}

// ---------------------------------------------------------------- BbkHid
BbkHid::BbkHid(QObject *parent) :
	QObject(parent),
	_connected(false),
	_registered(false),
	_mouseButtons(0)
{
	memset(_kbd, 0, sizeof(_kbd));
	memset(_mouse, 0, sizeof(_mouse));
}

BbkHid::~BbkHid()
{
	stop();
}

bool BbkHid::start(QString *error)
{
	bt_hid_callbacks_t cb;
	memset(&cb, 0, sizeof(cb));
	// Callbacks are mandatory: bt_hid_init fails EINVAL without them.
	cb.connected    = hidOnConnected;
	cb.disconnected = hidOnDisconnected;
	cb.report_set   = hidOnReportSet;

	qDebug("BBK: bt_hid_init...");
	if (bt_hid_init(&cb) != 0) {
		if (error)
			*error = QString::fromLatin1("bt_hid_init: %1")
			         .arg(QString::fromLatin1(strerror(errno)));
		return false;
	}

	g_hid = this;

	qDebug("BBK: bt_hid_register...");
	if (bt_hid_register() != 0) {
		if (error)
			*error = QString::fromLatin1("bt_hid_register: %1")
			         .arg(QString::fromLatin1(strerror(errno)));
		bt_hid_deinit();
		g_hid = 0;
		return false;
	}

	_registered = true;
	qDebug("BBK: HID device registered");
	return true;
}

bool BbkHid::connectTo(const QString &bdaddr, QString *error)
{
	if (!_registered) {
		if (error)
			*error = QString::fromLatin1("HID service not started");
		return false;
	}
	// Already connected: a second bt_hid_connect() answers EBUSY.
	if (_connected) {
		qDebug("BBK: already connected");
		return true;
	}
	if (bdaddr.trimmed().isEmpty()) {
		if (error)
			*error = QString::fromLatin1("no PC address given");
		return false;
	}

	const QByteArray addr = bdaddr.trimmed().toLatin1();
	qDebug("BBK: connecting to %s ...", addr.constData());

	// This call is what makes Windows show a HID device; just registering
	// leaves it showing AVRCP and nothing else.
	if (bt_hid_connect(addr.constData()) != 0) {
		if (error)
			*error = QString::fromLatin1("bt_hid_connect(%1): %2")
			         .arg(bdaddr)
			         .arg(QString::fromLatin1(strerror(errno)));
		qDebug("BBK: connect failed: %s", strerror(errno));
		return false;
	}

	// Do not wait for the connected callback: on this stack bt_hid_connect()
	// returns 0 and the link really is up (a second call answers EBUSY), but
	// the callback never arrives.  Relying on it left _connected false, so
	// every report was dropped before it reached the stack.
	_peer = bdaddr.trimmed();
	_connected = true;
	qDebug("BBK: connected to %s (link up)", addr.constData());
	emit peerConnected(_peer);
	return true;
}

void BbkHid::stop()
{
	if (_connected)
		bt_hid_disconnect();
	if (_registered)
		bt_hid_deregister();
	bt_hid_deinit();
	g_hid = 0;
	_registered = false;
	_connected = false;
}

void BbkHid::notifyConnected(const QString &addr)
{
	_connected = true;
	_peer = addr;
	emit peerConnected(_peer);
}

void BbkHid::forceDisconnect()
{
	if (!_connected)
		return;
	_connected = false;
	emit peerDisconnected();
}

bool BbkHid::probe()
{
	if (!_connected)
		return false;

	// An empty keyboard report: nothing is pressed, so it cannot change
	// anything on the PC, but the stack still has to route it.  A link
	// that has gone away fails here.
	unsigned char buf[9];
	memset(buf, 0, sizeof(buf));
	buf[0] = 0x01;                       // keyboard report id

	bt_hid_report_t rep;
	rep.reportType = BT_HID_REPORT_INPUT;
	rep.dataLen    = sizeof(buf);
	rep.data       = buf;
	return bt_hid_send_interrupt(&rep) == 0;
}

void BbkHid::notifyDisconnected()
{
	_connected = false;
	emit peerDisconnected();
}

void BbkHid::sendReport(int reportId, const unsigned char *data, int len)
{
	if (!_connected)
		return;

	bt_hid_report_t rep;
	rep.reportType = BT_HID_REPORT_INPUT;
	rep.dataLen    = (uint16_t)len;
	rep.data       = const_cast<uint8_t *>(data);
	Q_UNUSED(reportId);

	// The first few reports are logged: if the PC does nothing, this says
	// whether the problem is before the stack (no key ever reached us).
	static int sent = 0;
	const int rc = bt_hid_send_interrupt(&rep);
	if (++sent <= 12 || rc != 0)
		qDebug("BBK: report id=%d len=%d -> %d", data[0], len, rc);
}

void BbkHid::keyEvent(int qtKey, int qtModifiers, bool pressed)
{
	// A modifier is a key in its own right.  It has no HID usage code - it
	// is a bit in the report's first byte - so looking it up in the key table
	// found nothing and the event was dropped.  That made the phone's Shift
	// dead as a standalone key, which is exactly the tap Windows input
	// methods use to switch between Chinese and English.  It also left the
	// modifier latched on the PC when a Ctrl/GUI chord was let go, because
	// the release never produced a report either.
	int modBit = 0;
	switch (qtKey) {
	case Qt::Key_Shift:   modBit = 0x02; break;   // left Shift
	case Qt::Key_Control: modBit = 0x01; break;   // left Ctrl
	case Qt::Key_Alt:     modBit = 0x04; break;   // left Alt
	case Qt::Key_Meta:    modBit = 0x08; break;   // left GUI
	}
	if (modBit) {
		// Alt is the phone's symbol shifter, not a PC modifier.  Announcing
		// a bare Alt would put the PC's menu bar into keyboard mode before
		// every Alt+<key>; only its release is reported, so a chord cannot
		// leave Alt latched.
		if (pressed && qtKey == Qt::Key_Alt)
			return;
		if (pressed)
			_kbd[0] |= (unsigned char)modBit;
		else
			_kbd[0] &= (unsigned char)~modBit;
		sendKeyboard();
		return;
	}

	// Tab is missing from the Q20 layout; it lives on Alt+Space, the same
	// place BGShellBB10 puts it.
	if ((qtModifiers & Qt::AltModifier) && qtKey == Qt::Key_Space) {
		if (pressed) {
			// Let go of Alt first: if the Tab goes out while Alt is still
			// down the PC reads Alt+Tab and switches windows instead of
			// inserting a tab.
			_kbd[0] = 0;
			sendKeyboard();
			sendChord(0, 0x2b);
		}
		return;
	}

	const int code = qtKeyToHid(qtKey);
	if (code == 0)
		return;

	int slot = -1;
	for (int i = 0; i < 6; ++i) {
		if (_kbd[2 + i] == (unsigned char)code) {
			slot = i;
			break;
		}
	}
	if (slot < 0 && pressed) {
		for (int i = 0; i < 6; ++i) {
			if (_kbd[2 + i] == 0) {
				slot = i;
				break;
			}
		}
	}
	if (slot < 0)
		return;

	if (pressed)
		_kbd[2 + slot] = (unsigned char)code;
	else
		_kbd[2 + slot] = 0;

	qtModifiersToHid(qtModifiers, &_kbd[0]);
	sendKeyboard();
}

void BbkHid::sendChord(int modifierBits, int hidUsage)
{
	if (!_connected)
		return;

	unsigned char buf[9];
	buf[0] = 0x01;                       // keyboard report id
	buf[1] = (unsigned char)modifierBits;
	buf[2] = 0;                          // reserved
	buf[3] = (unsigned char)hidUsage;
	memset(buf + 4, 0, 5);
	sendReport(1, buf, sizeof(buf));

	// Release, so the next chord is independent of this one.
	memset(buf + 1, 0, 8);
	sendReport(1, buf, sizeof(buf));
}

void BbkHid::tapKey(int hidUsage, int modifierBits)
{
	sendChord(modifierBits, hidUsage);
}

// The stack's third report ("Enhanced Keys") is a bit field on the consumer
// page: Mute 0x01, VolumeUp 0x02, VolumeDown 0x04, Play/Pause 0x08,
// Stop 0x10, Next 0x20, Previous 0x40, Rewind 0x80.  The report is one
// byte of flags followed by seven constant bytes, and the report id goes
// in front of it - the same shape as the keyboard and mouse reports.
void BbkHid::tapMediaKey(int usage)
{
	if (!_connected)
		return;

	unsigned char buf[9];
	memset(buf, 0, sizeof(buf));
	buf[0] = 0x03;                       // report id: enhanced keys
	buf[1] = (unsigned char)usage;
	sendReport(3, buf, sizeof(buf));

	// Release, so the next key is independent of this one.
	buf[1] = 0;
	sendReport(3, buf, sizeof(buf));
}

void BbkHid::typeChar(int ascii, bool pressed)
{
	if (!_connected)
		return;

	unsigned char mods = 0;
	const int usage = asciiToHid(ascii, &mods);
	if (usage == 0)
		return;

	// Only the Shift that this character needs is touched.  The other
	// modifier bits belong to keys the user is holding (right Shift is
	// Ctrl, SYM is Win), and overwriting the whole byte dropped them.
	if (pressed)
		_kbd[0] |= mods;
	else
		_kbd[0] &= (unsigned char)~mods;

	int slot = -1;
	for (int i = 0; i < 6; ++i) {
		if (_kbd[2 + i] == (unsigned char)usage) {
			slot = i;
			break;
		}
	}
	if (slot < 0 && pressed) {
		for (int i = 0; i < 6; ++i) {
			if (_kbd[2 + i] == 0) {
				slot = i;
				break;
			}
		}
	}
	if (slot < 0)
		return;

	_kbd[2 + slot] = pressed ? (unsigned char)usage : 0;
	sendKeyboard();
}

void BbkHid::sendKeyboard()
{
	// The report id is the first byte of the payload for this stack.
	unsigned char buf[9];
	buf[0] = 0x01;
	memcpy(buf + 1, _kbd, 8);
	sendReport(1, buf, sizeof(buf));
}

void BbkHid::mouseMove(int dx, int dy)
{
	if (!_connected || (dx == 0 && dy == 0))
		return;

	int left = dx;
	int up   = dy;
	while (left != 0 || up != 0) {
		int sx = left;
		int sy = up;
		if (sx > 127)  sx = 127;  if (sx < -127) sx = -127;
		if (sy > 127)  sy = 127;  if (sy < -127) sy = -127;
		left -= sx;
		up   -= sy;

		_mouse[1] = (unsigned char)(sx & 0xff);
		_mouse[2] = (unsigned char)(sy & 0xff);
		sendMouse();
	}
	// Displacement is relative: sending it again would move the pointer a
	// second time.  Every later report (button, wheel) starts from zero.
	_mouse[1] = 0;
	_mouse[2] = 0;
}

void BbkHid::mouseButton(int button, bool pressed)
{
	if (!_connected)
		return;

	const int bit = (button == 1) ? 0x01 : (button == 2) ? 0x02 : 0x04;
	if (pressed)
		_mouseButtons |= bit;
	else
		_mouseButtons &= ~bit;

	_mouse[0] = (unsigned char)_mouseButtons;
	_mouse[1] = 0;                  // no leftover movement on a click
	_mouse[2] = 0;
	sendMouse();
}

void BbkHid::mouseWheel(int notches)
{
	if (!_connected || notches == 0)
		return;

	_mouse[1] = 0;                  // otherwise every notch also nudges the pointer
	_mouse[2] = 0;
	_mouse[3] = (unsigned char)(notches > 0 ? 1 : -1);
	sendMouse();
	_mouse[3] = 0;
}

// Horizontal scroll, sent as AC Pan (the last byte of the mouse report).
void BbkHid::mousePan(int notches)
{
	if (!_connected || notches == 0)
		return;

	_mouse[1] = 0;
	_mouse[2] = 0;
	_mouse[4] = (unsigned char)(notches > 0 ? 1 : -1);
	sendMouse();
	_mouse[4] = 0;
}

void BbkHid::mouseZoom(int notches)
{
	if (!_connected || notches == 0)
		return;

	// Ctrl is held in the keyboard report while the wheel turns in the
	// mouse report; the two are separate reports, so the modifier has to
	// go down first and come back up afterwards.
	const unsigned char saved = _kbd[0];
	_kbd[0] = (unsigned char)(saved | 0x01);       // Ctrl
	sendKeyboard();

	mouseWheel(notches);

	_kbd[0] = saved;
	sendKeyboard();
}

void BbkHid::sendMouse()
{
	unsigned char buf[6];
	buf[0] = 0x02;
	memcpy(buf + 1, _mouse, 5);
	sendReport(2, buf, sizeof(buf));
}
