/*
 *	Copyright (C) 2015,2016 Jonathan Naylor, G4KLX
 *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation; version 2 of the License.
 *
 *	This program is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *	GNU General Public License for more details.
 */

#include "YSFControl.h"
#include "YSFFICH.h"
#include "Utils.h"
#include "Sync.h"
#include "Log.h"

#include <cstdio>
#include <cassert>
#include <cstring>
#include <ctime>

// #define	DUMP_YSF

CYSFControl::CYSFControl(const std::string& callsign, CYSFNetwork* network, CDisplay* display, unsigned int timeout, bool duplex) :
m_network(network),
m_display(display),
m_duplex(duplex),
m_queue(5000U, "YSF Control"),
m_rfState(RS_RF_LISTENING),
m_netState(RS_NET_IDLE),
m_rfTimeoutTimer(1000U, timeout),
m_netTimeoutTimer(1000U, timeout),
m_networkWatchdog(1000U, 0U, 1500U),
m_holdoffTimer(1000U, 0U, 500U),
m_rfFrames(0U),
m_netFrames(0U),
m_rfErrs(0U),
m_rfBits(0U),
m_source(NULL),
m_dest(NULL),
m_payload(),
m_fp(NULL)
{
	assert(display != NULL);

	m_payload.setUplink(callsign);
	m_payload.setDownlink(callsign);
}

CYSFControl::~CYSFControl()
{
}

bool CYSFControl::writeModem(unsigned char *data)
{
	assert(data != NULL);

	unsigned char type = data[0U];

	if (type == TAG_LOST && m_rfState == RS_RF_AUDIO) {
		LogMessage("YSF, transmission lost, %.1f seconds, BER: %.1f%%", float(m_rfFrames) / 10.0F, float(m_rfErrs * 100U) / float(m_rfBits));
		writeEndRF();
		return false;
	}

	if (type == TAG_LOST)
		return false;

	CYSFFICH fich;
	bool valid = fich.decode(data + 2U);

	if (valid && m_rfState == RS_RF_LISTENING) {
		unsigned char fi = fich.getFI();
		if (fi == YSF_FI_TERMINATOR)
			return false;

		m_rfFrames = 0U;
		m_rfErrs = 0U;
		m_rfBits = 1U;
		m_rfTimeoutTimer.start();
		m_holdoffTimer.stop();
		m_payload.reset();
		m_rfState = RS_RF_AUDIO;
#if defined(DUMP_YSF)
		openFile();
#endif
	}

	if (m_rfState != RS_RF_AUDIO)
		return false;

	unsigned char fi = fich.getFI();
	if (valid && fi == YSF_FI_HEADER) {
		CSync::addYSFSync(data + 2U);

		m_rfFrames++;

		valid = m_payload.processHeaderData(data + 2U);

		data[0U] = TAG_DATA;
		data[1U] = 0x00U;

		writeNetwork(data);

		if (m_duplex) {
			fich.setMR(YSF_MR_BUSY);
			fich.encode(data + 2U);
			writeQueueRF(data);
		}

		if (valid)
			m_source = m_payload.getSource();

		unsigned char cm = fich.getCM();
		if (cm == YSF_CM_GROUP) {
			m_dest = (unsigned char*)"ALL       ";
		} else {
			if (valid)
				m_dest = m_payload.getDest();
		}

#if defined(DUMP_YSF)
		writeFile(data + 2U);
#endif

		if (m_source != NULL && m_dest != NULL) {
			m_display->writeFusion((char*)m_source, (char*)m_dest, "R");
			LogMessage("YSF, received RF header from %10.10s to %10.10s", m_source, m_dest);
		} else if (m_source == NULL && m_dest != NULL) {
			m_display->writeFusion("??????????", (char*)m_dest, "R");
			LogMessage("YSF, received RF header from ?????????? to %10.10s", m_dest);
		} else if (m_source != NULL && m_dest == NULL) {
			m_display->writeFusion((char*)m_source, "??????????", "R");
			LogMessage("YSF, received RF header from %10.10s to ??????????", m_source);
		} else {
			m_display->writeFusion("??????????", "??????????", "R");
			LogMessage("YSF, received RF header from ?????????? to ??????????");
		}
	} else if (valid && fi == YSF_FI_TERMINATOR) {
		CSync::addYSFSync(data + 2U);

		m_rfFrames++;

		m_payload.processHeaderData(data + 2U);

		data[0U] = TAG_EOT;
		data[1U] = 0x00U;

		writeNetwork(data);

		if (m_duplex) {
			fich.setMR(YSF_MR_BUSY);
			fich.encode(data + 2U);
			writeQueueRF(data);
		}

#if defined(DUMP_YSF)
		writeFile(data + 2U);
#endif

		LogMessage("YSF, received RF end of transmission, %.1f seconds, BER: %.1f%%", float(m_rfFrames) / 10.0F, float(m_rfErrs * 100U) / float(m_rfBits));
		writeEndRF();

		return false;
	} else if (valid) {
		CSync::addYSFSync(data + 2U);

		unsigned char fn = fich.getFN();
		unsigned char ft = fich.getFT();
		unsigned char dt = fich.getDT();

		m_rfFrames++;

		switch (dt) {
		case YSF_DT_VD_MODE1:
			valid = m_payload.processVDMode1Data(data + 2U, fn);
			m_rfErrs += m_payload.processVDMode1Audio(data + 2U);
			m_rfBits += 235U;
			break;

		case YSF_DT_VD_MODE2:
			valid = m_payload.processVDMode2Data(data + 2U, fn);
			m_rfErrs += m_payload.processVDMode2Audio(data + 2U);
			m_rfBits += 135U;
			break;

		case YSF_DT_DATA_FR_MODE:
			valid = m_payload.processDataFRModeData(data + 2U, fn);
			break;

		case YSF_DT_VOICE_FR_MODE:
			if (fn != 0U || ft != 1U) {
				// The first packet after the header is odd, don't try and regenerate it
				m_rfErrs += m_payload.processVoiceFRModeAudio(data + 2U);
				m_rfBits += 720U;
			}
			valid = false;
			break;

		default:
			break;
		}

		bool change = false;

		if (m_dest == NULL) {
			unsigned char cm = fich.getCM();
			if (cm == YSF_CM_GROUP) {
				m_dest = (unsigned char*)"ALL       ";
				change = true;
			} else if (valid) {
				m_dest = m_payload.getDest();
				if (m_dest != NULL)
					change = true;
			}
		}

		if (valid && m_source == NULL) {
			m_source = m_payload.getSource();
			if (m_source != NULL)
				change = true;
		}

		if (change) {
			if (m_source != NULL && m_dest != NULL) {
				m_display->writeFusion((char*)m_source, (char*)m_dest, "R");
				LogMessage("YSF, received RF data from %10.10s to %10.10s", m_source, m_dest);
			}
			if (m_source != NULL && m_dest == NULL) {
				m_display->writeFusion((char*)m_source, "??????????", "R");
				LogMessage("YSF, received RF data from %10.10s to ??????????", m_source);
			}
			if (m_source == NULL && m_dest != NULL) {
				m_display->writeFusion("??????????", (char*)m_dest, "R");
				LogMessage("YSF, received RF data from ?????????? to %10.10s", m_dest);
			}
		}

		data[0U] = TAG_DATA;
		data[1U] = 0x00U;

		writeNetwork(data);

		if (m_duplex) {
			fich.setMR(YSF_MR_BUSY);
			fich.encode(data + 2U);
			writeQueueRF(data);
		}

#if defined(DUMP_YSF)
		writeFile(data + 2U);
#endif
	} else {
		CSync::addYSFSync(data + 2U);

		m_rfFrames++;

		data[0U] = TAG_DATA;
		data[1U] = 0x00U;

		writeNetwork(data);

		if (m_duplex)
			writeQueueRF(data);

#if defined(DUMP_YSF)
		writeFile(data + 2U);
#endif
	}

	return true;
}

unsigned int CYSFControl::readModem(unsigned char* data)
{
	assert(data != NULL);

	if (m_queue.isEmpty())
		return 0U;

	// Don't relay data until the timer has stopped.
	if (m_holdoffTimer.isRunning())
		return 0U;

	unsigned char len = 0U;
	m_queue.getData(&len, 1U);

	m_queue.getData(data, len);

	return len;
}

void CYSFControl::writeEndRF()
{
	m_rfState = RS_RF_LISTENING;

	m_rfTimeoutTimer.stop();
	m_payload.reset();

	// These variables are free'd by YSFPayload
	m_source = NULL;
	m_dest = NULL;

	if (m_netState == RS_NET_IDLE) {
		m_display->clearFusion();

		if (m_network != NULL)
			m_network->reset();
	}

#if defined(DUMP_YSF)
	closeFile();
#endif
}

void CYSFControl::writeEndNet()
{
	m_netState = RS_NET_IDLE;

	m_netTimeoutTimer.stop();
	m_networkWatchdog.stop();

	m_display->clearFusion();

	if (m_network != NULL)
		m_network->reset();
}

void CYSFControl::writeNetwork()
{
	unsigned char data[200U];
	unsigned int length = m_network->read(data);
	if (length == 0U)
		return;

	if (m_rfState != RS_RF_LISTENING && m_netState == RS_NET_IDLE)
		return;

	m_networkWatchdog.start();

	if (!m_netTimeoutTimer.isRunning()) {
		m_display->writeFusion("??????????", "??????????", "N");
		LogMessage("YSF, received network data from ?????????? to ??????????");
		m_netTimeoutTimer.start();
		m_holdoffTimer.start();
		m_netState = RS_NET_AUDIO;
		m_netFrames = 0U;
	}

	m_netFrames++;

	writeQueueNet(data);

	if (data[0U] == TAG_EOT) {
		LogMessage("YSF, received network end of transmission, %.1f seconds", float(m_netFrames) / 10.0F);
		writeEndNet();
	}
}

void CYSFControl::clock(unsigned int ms)
{
	if (m_network != NULL)
		writeNetwork();

	m_holdoffTimer.clock(ms);
	if (m_holdoffTimer.isRunning() && m_holdoffTimer.hasExpired())
		m_holdoffTimer.stop();

	m_rfTimeoutTimer.clock(ms);
	m_netTimeoutTimer.clock(ms);

	if (m_netState == RS_NET_AUDIO) {
		m_networkWatchdog.clock(ms);

		if (m_networkWatchdog.hasExpired()) {
			LogMessage("YSF, network watchdog has expired, %.1f seconds", float(m_netFrames) / 10.0F);
			writeEndNet();
		}
	}
}

void CYSFControl::writeQueueRF(const unsigned char *data)
{
	assert(data != NULL);

	if (m_netState != RS_NET_IDLE)
		return;

	if (m_rfTimeoutTimer.isRunning() && m_rfTimeoutTimer.hasExpired())
		return;

	unsigned char len = YSF_FRAME_LENGTH_BYTES + 2U;

	unsigned int space = m_queue.freeSpace();
	if (space < (len + 1U)) {
		LogError("YSF, overflow in the System Fusion RF queue");
		return;
	}

	m_queue.addData(&len, 1U);

	m_queue.addData(data, len);
}

void CYSFControl::writeQueueNet(const unsigned char *data)
{
	assert(data != NULL);

	if (m_netTimeoutTimer.isRunning() && m_netTimeoutTimer.hasExpired())
		return;

	unsigned char len = YSF_FRAME_LENGTH_BYTES + 2U;

	unsigned int space = m_queue.freeSpace();
	if (space < (len + 1U)) {
		LogError("YSF, overflow in the System Fusion RF queue");
		return;
	}

	m_queue.addData(&len, 1U);

	m_queue.addData(data, len);
}

void CYSFControl::writeNetwork(const unsigned char *data)
{
	assert(data != NULL);

	if (m_network == NULL)
		return;

	if (m_rfTimeoutTimer.isRunning() && m_rfTimeoutTimer.hasExpired())
		return;

	m_network->write(m_source, m_dest, data + 2U, data[0U] == TAG_EOT);
}

bool CYSFControl::openFile()
{
	if (m_fp != NULL)
		return true;

	time_t t;
	::time(&t);

	struct tm* tm = ::localtime(&t);

	char name[100U];
	::sprintf(name, "YSF_%04d%02d%02d_%02d%02d%02d.ambe", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec);

	m_fp = ::fopen(name, "wb");
	if (m_fp == NULL)
		return false;

	::fwrite("YSF", 1U, 3U, m_fp);

	return true;
}

bool CYSFControl::writeFile(const unsigned char* data)
{
	if (m_fp == NULL)
		return false;

	::fwrite(data, 1U, YSF_FRAME_LENGTH_BYTES, m_fp);

	return true;
}

void CYSFControl::closeFile()
{
	if (m_fp != NULL) {
		::fclose(m_fp);
		m_fp = NULL;
	}
}
