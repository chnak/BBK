/*
 * BbkHid - the BlackBerry Q20 as a Bluetooth keyboard and mouse.
 *
 * Classic Bluetooth HID device role via QNX btapi/bthid.h.  The descriptor
 * is fixed by the stack:
 *
 *   Report ID 1  keyboard  8 bytes: mod, reserved, keycode x 6
 *   Report ID 2  mouse     5 bytes: buttons, x, y, wheel, ac-pan
 *
 * Two things are essential:
 *  1. bt_hid_init() needs real callbacks - an all-zero struct fails EINVAL.
 *  2. The phone must CONNECT to the host itself.  Waiting for the PC to open
 *     the channel leaves Windows showing only AVRCP, which is exactly what
 *     happened with the first version.
 */
#ifndef BBKHID_H
#define BBKHID_H

#include <stdint.h>
#include <QtCore/QObject>
#include <QtCore/QString>

class BbkHid : public QObject
{
	Q_OBJECT
public:
	explicit BbkHid(QObject *parent = 0);
	~BbkHid();

	bool start(QString *error);
	bool connectTo(const QString &bdaddr, QString *error);
	void stop();

	bool isConnected() const { return _connected; }
	QString peerName() const { return _peer; }

	/** Is the link really still up?  The stack never reports the peer
	 *  going away on this device, so the only way to find out is to send
	 *  something and see whether it goes through. */
	bool probe();

	void notifyConnected(const QString &addr);
	void notifyDisconnected();

	/** Press and release one HID usage, optionally with modifier bits
	 *  (0x01 Ctrl, 0x02 Shift, 0x04 Alt, 0x08 GUI).  Used by the shortcut
	 *  bar, which speaks HID directly rather than Qt keys. */
	void tapKey(int hidUsage, int modifierBits = 0);

	/** Press and release one consumer-page key (volume, mute, play, ...).
	 *  These live in the stack's third report, "Enhanced Keys". */
	void tapMediaKey(int usage);

	/** Type one printable ASCII character.  The phone's Alt layout is
	 *  resolved to the character it produces (Alt+E is "2"), so only the
	 *  character - and any Shift it needs - reaches the PC. */
	void typeChar(int ascii, bool pressed);

	/** Shift+Alt+<letter> is Ctrl+<letter> on a BlackBerry keyboard. */
	void sendChord(int modifierBits, int hidUsage);

public slots:
	void keyEvent(int qtKey, int qtModifiers, bool pressed);
	void mouseMove(int dx, int dy);
	void mouseButton(int button, bool pressed);
	void mouseWheel(int notches);
	void mousePan(int notches);

	/** Zoom, which most programs expect as Ctrl plus the wheel.  The
	 *  modifier lives in the keyboard report and the wheel in the mouse
	 *  report, so Ctrl is held for the duration of the wheel turn. */
	void mouseZoom(int notches);

signals:
	void peerConnected(const QString &name);
	void peerDisconnected();

private:
	void sendReport(int reportId, const unsigned char *data, int len);
	void sendKeyboard();
	void sendMouse();

public:
	/** Forget the peer, so the next connectTo() tries again. */
	void forceDisconnect();

	bool _connected;
	bool _registered;
	QString _peer;
	unsigned char _kbd[8];
	unsigned char _mouse[5];
	int  _mouseButtons;
};

#endif
