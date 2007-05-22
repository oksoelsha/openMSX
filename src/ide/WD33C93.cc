// $Id$
/* Ported from:
** Source: /cvsroot/bluemsx/blueMSX/Src/IoDevice/wd33c93.c,v
** $Revision: 1.12 $
** $Date$
**
** Based on the WD33C93 emulation in MESS (www.mess.org).
**
** More info: http://www.bluemsx.com
**
** Copyright (C) 2003-2006 Daniel Vik, Tomas Karlsson, white cat
**
*/

#include "WD33C93.hh"
#include "SCSI.hh"
#include "SCSIDevice.hh"
#include "XMLElement.hh"
#include <cassert>
#include <cstring>
#include <iostream>
#undef PRT_DEBUG
#define  PRT_DEBUG(mes)                          \
        do {                                    \
                std::cout << mes << std::endl;  \
        } while (0)

namespace openmsx {

static const int MAX_DEV = 8;

static const byte REG_OWN_ID      = 0x00;
static const byte REG_CONTROL     = 0x01;
static const byte REG_TIMEO       = 0x02;
static const byte REG_TSECS       = 0x03;
static const byte REG_THEADS      = 0x04;
static const byte REG_TCYL_HI     = 0x05;
static const byte REG_TCYL_LO     = 0x06;
static const byte REG_ADDR_HI     = 0x07;
static const byte REG_ADDR_2      = 0x08;
static const byte REG_ADDR_3      = 0x09;
static const byte REG_ADDR_LO     = 0x0a;
static const byte REG_SECNO       = 0x0b;
static const byte REG_HEADNO      = 0x0c;
static const byte REG_CYLNO_HI    = 0x0d;
static const byte REG_CYLNO_LO    = 0x0e;
static const byte REG_TLUN        = 0x0f;
static const byte REG_CMD_PHASE   = 0x10;
static const byte REG_SYN         = 0x11;
static const byte REG_TCH         = 0x12;
static const byte REG_TCM         = 0x13;
static const byte REG_TCL         = 0x14;
static const byte REG_DST_ID      = 0x15;
static const byte REG_SRC_ID      = 0x16;
static const byte REG_SCSI_STATUS = 0x17; // (r)
static const byte REG_CMD         = 0x18;
static const byte REG_DATA        = 0x19;
static const byte REG_QUEUE_TAG   = 0x1a;
static const byte REG_AUX_STATUS  = 0x1f; // (r)

static const byte REG_CDBSIZE     = 0x00;
static const byte REG_CDB1        = 0x03;
static const byte REG_CDB2        = 0x04;
static const byte REG_CDB3        = 0x05;
static const byte REG_CDB4        = 0x06;
static const byte REG_CDB5        = 0x07;
static const byte REG_CDB6        = 0x08;
static const byte REG_CDB7        = 0x09;
static const byte REG_CDB8        = 0x0a;
static const byte REG_CDB9        = 0x0b;
static const byte REG_CDB10       = 0x0c;
static const byte REG_CDB11       = 0x0d;
static const byte REG_CDB12       = 0x0e;

static const byte OWN_EAF         = 0x08; // ENABLE ADVANCED FEATURES

// SCSI STATUS
static const byte SS_RESET        = 0x00; // reset
static const byte SS_RESET_ADV    = 0x01; // reset w/adv. features
static const byte SS_XFER_END     = 0x16; // select and transfer complete
static const byte SS_SEL_TIMEOUT  = 0x42; // selection timeout
static const byte SS_DISCONNECT   = 0x85;

// AUX STATUS
static const byte AS_DBR          = 0x01; // data buffer ready
static const byte AS_CIP          = 0x10; // command in progress, chip is busy
static const byte AS_BSY          = 0x20; // Level 2 command in progress
static const byte AS_LCI          = 0x40; // last command ignored
static const byte AS_INT          = 0x80;

/* command phase
0x00    NO_SELECT
0x10    SELECTED
0x20    IDENTIFY_SENT
0x30    COMMAND_OUT
0x41    SAVE_DATA_RECEIVED
0x42    DISCONNECT_RECEIVED
0x43    LEGAL_DISCONNECT
0x44    RESELECTED
0x45    IDENTIFY_RECEIVED
0x46    DATA_TRANSFER_DONE
0x47    STATUS_STARTED
0x50    STATUS_RECEIVED
0x60    COMPLETE_RECEIVED
*/

WD33C93::WD33C93(const XMLElement& config)
{
	buffer = (byte*)malloc(SCSIDevice::BUFFER_SIZE);
	devBusy = false;

	XMLElement::Children targets;
	config.getChildren("target", targets);
	for (XMLElement::Children::const_iterator it = targets.begin(); 
	     it != targets.end(); ++it) {
		const XMLElement& target = **it;
		int id = target.getAttributeAsInt("id");
		assert(id < MAX_DEV); // TODO should not assert on invalid input
		const XMLElement& typeElem = target.getChild("type");
		const std::string& type = typeElem.getData();
		(void)type;
		assert(type == "SCSIHD"); // we only do SCSIHD for now
   
		dev[id].reset(new SCSIDevice(id, buffer, NULL, SCSI::DT_DirectAccess,
		                SCSIDevice::MODE_SCSI1 | SCSIDevice::MODE_UNITATTENTION |
		                SCSIDevice::MODE_FDS120 | SCSIDevice::MODE_REMOVABLE |
		                SCSIDevice::MODE_NOVAXIS));
	}
	reset(false);
}

WD33C93::~WD33C93()
{
	PRT_DEBUG("WD33C93 destroy");
	free(buffer);
}

void WD33C93::disconnect()
{
	if (phase != SCSI::BUS_FREE) {
		assert(targetId < MAX_DEV);
		dev[targetId]->disconnect();
		if (regs[REG_SCSI_STATUS] != SS_XFER_END) {
			regs[REG_SCSI_STATUS] = SS_DISCONNECT;
		}
		regs[REG_AUX_STATUS] = AS_INT;
		phase = SCSI::BUS_FREE;
	}
	tc = 0;

	PRT_DEBUG("busfree()");
}

void WD33C93::execCmd(byte value)
{
	if (regs[REG_AUX_STATUS] & AS_CIP) {
		PRT_DEBUG("wd33c93ExecCmd() CIP error");
		return;
	}
	//regs[REG_AUX_STATUS] |= AS_CIP;
	regs[REG_CMD] = value;
	
	bool atn = false;
	switch (value) {
	case 0x00: // Reset controller (software reset)
		PRT_DEBUG("wd33c93 [CMD] Reset controller");
		memset(regs + 1, 0, 0x1a);
		disconnect();
		latch = 0;
		regs[REG_SCSI_STATUS] =
			(regs[REG_OWN_ID] & OWN_EAF) ? SS_RESET_ADV : SS_RESET;
		break;

	case 0x02: // Assert ATN
		PRT_DEBUG("wd33c93 [CMD] Assert ATN");
		break;

	case 0x04: // Disconnect
		PRT_DEBUG("wd33c93 [CMD] Disconnect");
		disconnect();
		break;

	case 0x06: // Select with ATN (Lv2)
		atn = true;
		// fall-through
	case 0x07: // Select Without ATN (Lv2)
		PRT_DEBUG("wd33c93 [CMD] Select (ATN " << atn << ")");
		targetId = regs[REG_DST_ID] & 7;
		regs[REG_SCSI_STATUS] = SS_SEL_TIMEOUT;
		tc = 0;
		regs[REG_AUX_STATUS] = AS_INT;
		break;

	case 0x08: // Select with ATN and transfer (Lv2)
		atn = true;
		// fall-through
	case 0x09: // Select without ATN and Transfer (Lv2)
		PRT_DEBUG("wd33c93 [CMD] Select and transfer (ATN " << atn << ")");
		targetId = regs[REG_DST_ID] & 7;

		if (!devBusy && targetId < MAX_DEV && /* targetId != myId  && */ 
		    dev[targetId].get() && // TODO: use dummy
		    dev[targetId]->isSelected()) {
			if (atn) {
				dev[targetId]->msgOut(regs[REG_TLUN] | 0x80);
			}
			devBusy = true; 
			counter = dev[targetId]->executeCmd(&regs[REG_CDB1],
			                               &phase, &blockCounter);

			switch (phase) {
			case SCSI::STATUS:
				devBusy = false;
				regs[REG_TLUN] = dev[targetId]->getStatusCode();
				dev[targetId]->msgIn();
				regs[REG_SCSI_STATUS] = SS_XFER_END;
				disconnect();
				break;

			case SCSI::EXECUTE:
				regs[REG_AUX_STATUS] = AS_CIP | AS_BSY;
				pBuf = buffer;
				break;

			default:
				devBusy = false;
				regs[REG_AUX_STATUS] = AS_CIP | AS_BSY | AS_DBR;
				pBuf = buffer;
			}
			//regs[REG_SRC_ID] |= regs[REG_DST_ID] & 7;
		} else {
			PRT_DEBUG("wd33c93 timeout on target " << (int)targetId);
			tc = 0;
			regs[REG_SCSI_STATUS] = SS_SEL_TIMEOUT;
			regs[REG_AUX_STATUS]  = AS_INT;
		}
		break;

	case 0x18: // Translate Address (Lv2)
	default:
		PRT_DEBUG("wd33c93 [CMD] unsupport command " << (int)value);
		break;
	}
}

void WD33C93::writeAdr(byte value)
{
	//PRT_DEBUG("WriteAdr value " << std::hex << (int)value);
	latch = value & 0x1f;
}

void WD33C93::writeCtrl(byte value)
{
	//PRT_DEBUG("wd33c93 write #" << std::hex << (int)latch << ", " << (int)value);
	switch (latch) {
	case REG_OWN_ID:
		regs[REG_OWN_ID] = value;
		myId = value & 7;
		PRT_DEBUG("wd33c93 myid = " << (int)myId);
		break;

	case REG_TCH:
		tc = (tc & 0x00ffff) + (value << 16);
		break;

	case REG_TCM:
		tc = (tc & 0xff00ff) + (value <<  8);
		break;

	case REG_TCL:
		tc = (tc & 0xffff00) + (value <<  0);
		break;

	case REG_CMD_PHASE:
		PRT_DEBUG("wd33c93 CMD_PHASE = " << (int)value);
		regs[REG_CMD_PHASE] = value;
		break;

	case REG_CMD:
		execCmd(value);
		return; // TODO no latch-inc ???

	case REG_DATA:
		regs[REG_DATA] = value;
		if (phase == SCSI::DATA_OUT) {
			*pBuf++ = value;
			--tc;
			if (--counter == 0) {
				counter = dev[targetId]->dataOut(&blockCounter);
				if (counter) {
					pBuf = buffer;
					return;
				}
				regs[REG_TLUN] = dev[targetId]->getStatusCode();
				dev[targetId]->msgIn();
				regs[REG_SCSI_STATUS] = SS_XFER_END;
				disconnect();
			}
		}
		return; // TODO no latch-inc ???

	case REG_AUX_STATUS:
		return; // TODO no latch-inc ???

	default:
		if (latch <= REG_SRC_ID) {
			regs[latch] = value;
		}
		break;
	}
	latch = (latch + 1) & 0x1f;
}

byte WD33C93::readAuxStatus()
{
	byte rv = regs[REG_AUX_STATUS];

	if (phase == SCSI::EXECUTE) {
		counter = dev[targetId]->executingCmd(&phase, &blockCounter);

		switch (phase) {
		case SCSI::STATUS: // TODO how can this ever be the case?
			regs[REG_TLUN] = dev[targetId]->getStatusCode();
			dev[targetId]->msgIn();
			regs[REG_SCSI_STATUS] = SS_XFER_END;
			disconnect();
			break;

		case SCSI::EXECUTE:
			break;

		default:
			regs[REG_AUX_STATUS] |= AS_DBR;
		}
	}
	//PRT_DEBUG("readAuxStatus returning " << std::hex << (int)rv);
	return rv;
}

byte WD33C93::readCtrl()
{
	//PRT_DEBUG("ReadCtrl");
	byte rv;

	switch (latch) {
	case REG_SCSI_STATUS:
		rv = regs[REG_SCSI_STATUS];
		//PRT_DEBUG1("wd33c93 SCSI_STATUS = %X\n", rv);
		if (rv != SS_XFER_END) {
			regs[REG_AUX_STATUS] &= ~AS_INT;
		} else {
			regs[REG_SCSI_STATUS] = SS_DISCONNECT;
			regs[REG_AUX_STATUS]  = AS_INT;
		}
		break;

	case REG_CMD:
		return regs[REG_CMD]; // note: don't increase latch

	case REG_DATA:
		if (phase == SCSI::DATA_IN) {
			rv = *pBuf++;
			regs[REG_DATA] = rv;
			--tc;
			if (--counter == 0) {
				if (blockCounter > 0) {
					counter = dev[targetId]->dataIn(&blockCounter);
					if (counter) {
						pBuf = buffer;
						return rv;
					}
				}
				regs[REG_TLUN] = dev[targetId]->getStatusCode();
				dev[targetId]->msgIn();
				regs[REG_SCSI_STATUS] = SS_XFER_END;
				disconnect();
			}
		} else {
			rv = regs[REG_DATA];
		}
		return rv; // TODO no latch-inc ???

	case REG_TCH:
		rv = (tc >> 16) & 0xff;
		break;

	case REG_TCM:
		rv = (tc >>  8) & 0xff;
		break;

	case REG_TCL:
		rv = (tc >>  0) & 0xff;
		break;

	case REG_AUX_STATUS:
		return readAuxStatus(); // TODO no latch-inc ???

	default:
		rv = regs[latch];
		break;
	}
	//PRT_DEBUG2("wd33c93 read #%X, %X\n", latch, rv);

	latch = (latch + 1) & 0x1f;
	return rv;
}

byte WD33C93::peekAuxStatus() const
{
	return regs[REG_AUX_STATUS];
}

byte WD33C93::peekCtrl() const
{
	switch (latch) {
	case REG_TCH:
		return (tc >> 16) & 0xff;
	case REG_TCM:
		return (tc >>  8) & 0xff;
	case REG_TCL:
		return (tc >>  0) & 0xff;
	default:
		return regs[latch];
	}
}

void WD33C93::reset(bool scsireset)
{
	PRT_DEBUG("wd33c93 reset");

	// initialized register
	memset(regs, 0, 0x1b);
	memset(regs + 0x1b, 0xff, 4);
	regs[REG_AUX_STATUS] = AS_INT;
	myId  = 0;
	latch = 0;
	tc    = 0;
	phase = SCSI::BUS_FREE;
	pBuf  = buffer;
	if (scsireset) {
		for (int i = 0; i < MAX_DEV; ++i) {
			if (dev[i].get()) dev[i]->reset(); // TODO: use Dummy
		}
	}
}

/* Here is some info on the parameters for SCSI devices:
static SCSIDevice* wd33c93ScsiDevCreate(WD33C93* wd33c93, int id)
{
#if 1
    // CD_UPDATE: Use dynamic parameters instead of hard coded ones
    int diskId, mode, type;

    diskId = diskGetHdDriveId(hdId, id);
    if (diskIsCdrom(diskId)) {
        mode = MODE_SCSI1 | MODE_UNITATTENTION | MODE_REMOVABLE | MODE_NOVAXIS;
        type = SDT_CDROM;
    } else {
        mode = MODE_SCSI1 | MODE_UNITATTENTION | MODE_FDS120 | MODE_REMOVABLE | MODE_NOVAXIS;
        type = SDT_DirectAccess;
    }
    return scsiDeviceCreate(id, diskId, buffer, NULL, type, mode,
                           (CdromXferCompCb)wd33c93XferCb, wd33c93);
#else
    SCSIDEVICE* dev;
    int mode;
    int type;

    if (id != 2) {
        mode = MODE_SCSI1 | MODE_UNITATTENTION | MODE_FDS120 | MODE_REMOVABLE | MODE_NOVAXIS;
        type = SDT_DirectAccess;
    } else {
        mode = MODE_SCSI1 | MODE_UNITATTENTION | MODE_REMOVABLE | MODE_NOVAXIS;
        type = SDT_CDROM;
    }
    dev = scsiDeviceCreate(id, diskGetHdDriveId(hdId, id),
            buffer, NULL, type, mode, (CdromXferCompCb)wd33c93XferCb, wd33c93);
    return dev;
#endif
}
*/

} // namespace openmsx
