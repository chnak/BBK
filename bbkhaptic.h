/*
 * Haptic feedback for BBK.
 *
 * The phone has a vibration motor and BlackBerry Platform Services exposes
 * it (bps/vibration.h).  A short buzz confirms that a key or a button was
 * accepted - useful on a phone used as a keyboard, where the PC's own
 * response is often out of sight.
 */
#ifndef BBKHAPTIC_H
#define BBKHAPTIC_H

#include <stdint.h>

#include <bps/vibration.h>

class BbkHaptic
{
public:
	BbkHaptic();

	/** Is there a vibration motor to talk to? */
	static bool isSupported();

	/** Buzz for a moment.  Returns false when the device has no motor or
	 *  the request was refused. */
	static bool buzz(int milliseconds, int intensity);

	/** The short "that worked" tick. */
	static bool tick();

	/** The longer "that did not work" buzz. */
	static bool error();
};

#endif
