/*
 * See bbkhaptic.h.
 */
#include <errno.h>
#include <stdio.h>

#include <bps/vibration.h>

#include <QtCore/QDebug>

#include "bbkhaptic.h"

BbkHaptic::BbkHaptic()
{
}

bool BbkHaptic::isSupported()
{
	return vibration_is_supported();
}

bool BbkHaptic::buzz(int milliseconds, int intensity)
{
	if (!vibration_is_supported())
		return false;
	if (vibration_request(intensity, milliseconds) != BPS_SUCCESS) {
		qDebug("BBK: vibration refused: %s", strerror(errno));
		return false;
	}
	return true;
}

bool BbkHaptic::tick()
{
	// A short, light buzz: long enough to feel, short enough not to
	// become annoying when typing quickly.
	return buzz(12, VIBRATION_INTENSITY_LOW);
}

bool BbkHaptic::error()
{
	return buzz(120, VIBRATION_INTENSITY_MEDIUM);
}
