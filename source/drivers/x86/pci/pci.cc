/**
 * $Id$
 * Copyright (C) 2008 - 2014 Nils Asmussen
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <esc/common.h>
#include <esc/driver.h>
#include <esc/debug.h>
#include <esc/messages.h>
#include <ipc/device.h>
#include <ipc/proto/pci.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>

#include "list.h"
#include "pci.h"

#define DEBUG	0

#if DEBUG
#	define DBG(fmt...)		print(fmt)
#else
#	define DBG(...)
#endif

using namespace ipc;

class PCIService : public Device {
public:
	explicit PCIService(const char *name,mode_t mode)
		: Device(name,mode,DEV_TYPE_SERVICE,DEV_CLOSE) {
		set(MSG_PCI_GET_BY_CLASS,std::make_memfun(this,&PCIService::getByClass));
		set(MSG_PCI_GET_BY_ID,std::make_memfun(this,&PCIService::getById));
		set(MSG_PCI_GET_BY_INDEX,std::make_memfun(this,&PCIService::getByIndex));
		set(MSG_PCI_GET_COUNT,std::make_memfun(this,&PCIService::getCount));
		set(MSG_PCI_READ,std::make_memfun(this,&PCIService::read));
		set(MSG_PCI_WRITE,std::make_memfun(this,&PCIService::write));
	}

	void getByClass(IPCStream &is) {
		int no;
		uchar cls,subcls;
		is >> cls >> subcls >> no;

		PCI::Device *d = list_getByClass(cls,subcls,no);
		reply(is,d);
	}

	void getById(IPCStream &is) {
		uchar bus,dev,func;
		is >> bus >> dev >> func;

		PCI::Device *d = list_getById(bus,dev,func);
		reply(is,d);
	}

	void getByIndex(IPCStream &is) {
		size_t idx;
		is >> idx;

		PCI::Device *d = list_get(idx);
		reply(is,d);
	}

	void getCount(IPCStream &is) {
		ssize_t len = list_length();
		is << len << Reply();
	}

	void read(IPCStream &is) {
		uchar bus,dev,func;
		uint32_t offset,value;
		is >> bus >> dev >> func >> offset;

		value = pci_read(bus,dev,func,offset);
		DBG("%02x:%02x:%x [%#04x] -> %#08x",bus,dev,func,offset,value);
		is << value << Reply();
	}

	void write(IPCStream &is) {
		uchar bus,dev,func;
		uint32_t offset,value;
		is >> bus >> dev >> func >> offset >> value;

		DBG("%02x:%02x:%x [%#04x] <- %#08x",bus,dev,func,offset,value);
		pci_write(bus,dev,func,offset,value);
	}

private:
	void reply(IPCStream &is,PCI::Device *d) {
		if(d)
			is << 0 << *d << Reply();
		else
			is << -ENOTFOUND << Reply();
	}
};

int main(void) {
	list_init();

	PCIService dev("/dev/pci",0111);
	dev.loop();
	return EXIT_SUCCESS;
}
