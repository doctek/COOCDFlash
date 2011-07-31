/***************************************************************************
 *   Copyright (C) 2005 by Dominic Rath                                    *
 *   Dominic.Rath@gmx.de                                                   *
 *                                                                         *
 *   Copyright (C) 2008 by Spencer Oliver                                  *
 *   spen@spen-soft.co.uk
 *
 *   Copyright (C) 2011 by James K. Larson                                 *
 *   jlarson@pacifier.com                                                  *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/
// This version of the stm32x flash driver has the same functionality as the OpenOCD
//  version of the same name (without "_doc"). This version simply includes
//  extensive commenting as a guide to developing custom flash drivers for 
//  other chips.
// Note that the name, stm32x, may be changed in future OpenOCD releases. This
//  driver is for ST Microelectronics ARM processors of the 32F1xx flavor.

// Files config.h and imp.h should always be included.
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "imp.h"
// The next three includes are specifically needed for this driver. They may or
//   may not be needed in drivers for other chips.
#include <helper/binarybuffer.h>
#include <target/algorithm.h>
#include <target/armv7m.h>

// The following defines are specific to the STM321xx chip. They should be replaced
//   with meaningful defines applicable to another target chip.
/* stm32x register locations */

#define STM32_FLASH_ACR		0x40022000
#define STM32_FLASH_KEYR	0x40022004
#define STM32_FLASH_OPTKEYR	0x40022008
#define STM32_FLASH_SR		0x4002200C
#define STM32_FLASH_CR		0x40022010
#define STM32_FLASH_AR		0x40022014
#define STM32_FLASH_OBR		0x4002201C
#define STM32_FLASH_WRPR	0x40022020

/* option byte location */

#define STM32_OB_RDP		0x1FFFF800
#define STM32_OB_USER		0x1FFFF802
#define STM32_OB_DATA0		0x1FFFF804
#define STM32_OB_DATA1		0x1FFFF806
#define STM32_OB_WRP0		0x1FFFF808
#define STM32_OB_WRP1		0x1FFFF80A
#define STM32_OB_WRP2		0x1FFFF80C
#define STM32_OB_WRP3		0x1FFFF80E

/* FLASH_CR register bits */

#define FLASH_PG		(1 << 0)
#define FLASH_PER		(1 << 1)
#define FLASH_MER		(1 << 2)
#define FLASH_OPTPG		(1 << 4)
#define FLASH_OPTER		(1 << 5)
#define FLASH_STRT		(1 << 6)
#define FLASH_LOCK		(1 << 7)
#define FLASH_OPTWRE	(1 << 9)

/* FLASH_SR register bits */

#define FLASH_BSY		(1 << 0)
#define FLASH_PGERR		(1 << 2)
#define FLASH_WRPRTERR	(1 << 4)
#define FLASH_EOP		(1 << 5)

/* STM32_FLASH_OBR bit definitions (reading) */

#define OPT_ERROR		0
#define OPT_READOUT		1
#define OPT_RDWDGSW		2
#define OPT_RDRSTSTOP	3
#define OPT_RDRSTSTDBY	4
#define OPT_BFB2		5	/* dual flash bank only */

/* register unlock keys */

#define KEY1			0x45670123
#define KEY2			0xCDEF89AB

// The following bank scheme is specific to the ST 32F1xx chip. Other chips
//    may or may not need or want to use it.
/* we use an offset to access the second bank on dual flash devices
 * strangely the protection of the second bank is done on the bank0 reg's */

#define FLASH_OFFSET_B0	0x00
#define FLASH_OFFSET_B1 0x40

// This structure is used to keep track of specific options for the stm32x.
//    Other chips may not need this or will have different options.
struct stm32x_options
{
	uint16_t RDP;
	uint16_t user_options;
	uint16_t protection[4];
};

// This is an important structure and most (probably all) chips will 
//    want their own version of this. It holds information needed by
//    any operations the driver will perform on any bank. A pointer to
//    this structure (appropriately filled in) is stored as private data
//    in the generic structure for each bank.
// Details:
//	The options bytes are specific to the stm32x and may not apply to
//	    other chips.
//	The write algorithm pointer points to the code which will be loaded
//	    into the chip sram to program the chip flash quickly. Most chips
//	    will need this.
//	Ppage size is the protection page size for those chips which have 
//	    protection implemented in sizes other than the sector size. This
//	    doesn't apply to many chips.
//	The probed flag just indicates whether or not the flash has been probed
//	    (identified) or not. This is very useful and should probably be in
//	    all implementations.
//	Dual banks and a register offset are meaningful in only the largest of
//	    the stm32x chips. This is often not required.
struct stm32x_flash_bank
{
	struct stm32x_options option_bytes;
	struct working_area *write_algorithm;
	int ppage_size;
	int probed;

	bool has_dual_banks;
	/* used to access dual flash bank stm32xl
	 * 0x00 will address bank 0 flash
	 * 0x40 will address bank 1 flash */
	int register_offset;
};

// Forward declaration of the mass erase function. Provide if
//	necessary. See discussion of mass erase below.
static int stm32x_mass_erase(struct flash_bank *bank);

// The flash bank command handler is a key function and will be discussed
//  at length. This command is unique; while it configures the private
//  bank structure and must be entered in the flash_driver structure at
//  the end of this file, it doesn't really relate to any other parts 
//  of this file.
// It's important to know that bank is passed in to this function (via
//  the magic of macros, I think?)
// This function is invoked by a statement in the config file as 
//  discussed in the documentation for this project. So it runs
//  only once, before the init function, and may *only* be called
//  from the config file. The next comment shows the format.
/* flash bank stm32x <base> <size> 0 0 <target#>
 */
FLASH_BANK_COMMAND_HANDLER(stm32x_flash_bank_command)
{
	struct stm32x_flash_bank *stm32x_info;

	// The "standard" command has six parameters. If additional ones
	//  are needed for a specific part, then this limit must be changed.
	if (CMD_ARGC < 6)
	{
		LOG_WARNING("incomplete flash_bank stm32x configuration");
		return ERROR_FLASH_BANK_INVALID;
	}

	// Here the space for part-specific bank information is allocated
	//  and the pointer is placed in the driver_priv (private) area
	//  of the bank structure.
	stm32x_info = malloc(sizeof(struct stm32x_flash_bank));
	bank->driver_priv = stm32x_info;

	// Now the private data structure is initialized. This
	//  data is (not surprisingly) specific to a particular device.
	stm32x_info->write_algorithm = NULL;
	stm32x_info->probed = 0;
	stm32x_info->has_dual_banks = false;
	stm32x_info->register_offset = FLASH_OFFSET_B0;

	return ERROR_OK;
}

// This helper function is only useful if multiple banks are applicable.
static inline int stm32x_get_flash_reg(struct flash_bank *bank, uint32_t reg)
{
	struct stm32x_flash_bank *stm32x_info = bank->driver_priv;
	return reg + stm32x_info->register_offset;
}
// This helper function is specific to the stm32x. Other chips may or may not use this method.
static inline int stm32x_get_flash_status(struct flash_bank *bank, uint32_t *status)
{
	struct target *target = bank->target;
	return target_read_u32(target, stm32x_get_flash_reg(bank, STM32_FLASH_SR), status);
}
// This helper function is specific to the stm32x. Other chips may or may not use this method.
static int stm32x_wait_status_busy(struct flash_bank *bank, int timeout)
{
	struct target *target = bank->target;
	uint32_t status;
	int retval = ERROR_OK;

	/* wait for busy to clear */
	for (;;)
	{
		retval = stm32x_get_flash_status(bank, &status);
		if (retval != ERROR_OK)
			return retval;
		LOG_DEBUG("status: 0x%" PRIx32 "", status);
		if ((status & FLASH_BSY) == 0)
			break;
		if (timeout-- <= 0)
		{
			LOG_ERROR("timed out waiting for flash");
			return ERROR_FAIL;
		}
		alive_sleep(1);
	}

	if (status & FLASH_WRPRTERR)
	{
		LOG_ERROR("stm32x device protected");
		retval = ERROR_FAIL;
	}

	if (status & FLASH_PGERR)
	{
		LOG_ERROR("stm32x device programming failed");
		retval = ERROR_FAIL;
	}

	/* Clear but report errors */
	if (status & (FLASH_WRPRTERR | FLASH_PGERR))
	{
		/* If this operation fails, we ignore it and report the original
		 * retval
		 */
		target_write_u32(target, stm32x_get_flash_reg(bank, STM32_FLASH_SR),
				FLASH_WRPRTERR | FLASH_PGERR);
	}
	return retval;
}

// stm32x specific function used to check the option byte.
int stm32x_check_operation_supported(struct flash_bank *bank)
{
	struct stm32x_flash_bank *stm32x_info = bank->driver_priv;

	/* if we have a dual flash bank device then
	 * we need to perform option byte stuff on bank0 only */
	if (stm32x_info->register_offset != FLASH_OFFSET_B0)
	{
		LOG_ERROR("Option Byte Operation's must use bank0");
		return ERROR_FLASH_OPERATION_FAILED;
	}

	return ERROR_OK;
}
// stm32x specific function to read the option byte.
static int stm32x_read_options(struct flash_bank *bank)
{
	uint32_t optiondata;
	struct stm32x_flash_bank *stm32x_info = NULL;
	struct target *target = bank->target;

	stm32x_info = bank->driver_priv;

	/* read current option bytes */
	int retval = target_read_u32(target, STM32_FLASH_OBR, &optiondata);
	if (retval != ERROR_OK)
		return retval;

	stm32x_info->option_bytes.user_options = (uint16_t)0xFFF8 | ((optiondata >> 2) & 0x07);
	stm32x_info->option_bytes.RDP = (optiondata & (1 << OPT_READOUT)) ? 0xFFFF : 0x5AA5;

	if (optiondata & (1 << OPT_READOUT))
		LOG_INFO("Device Security Bit Set");

	/* each bit refers to a 4bank protection */
	retval = target_read_u32(target, STM32_FLASH_WRPR, &optiondata);
	if (retval != ERROR_OK)
		return retval;

	stm32x_info->option_bytes.protection[0] = (uint16_t)optiondata;
	stm32x_info->option_bytes.protection[1] = (uint16_t)(optiondata >> 8);
	stm32x_info->option_bytes.protection[2] = (uint16_t)(optiondata >> 16);
	stm32x_info->option_bytes.protection[3] = (uint16_t)(optiondata >> 24);

	return ERROR_OK;
}
// stm32x specific function to erase the option byte. On the ST chip, this allows
//	reprogramming. Other chips will have other methods of doing this.
static int stm32x_erase_options(struct flash_bank *bank)
{
	struct stm32x_flash_bank *stm32x_info = NULL;
	struct target *target = bank->target;

	stm32x_info = bank->driver_priv;

	/* read current options */
	stm32x_read_options(bank);

	/* unlock flash registers */
	int retval = target_write_u32(target, STM32_FLASH_KEYR, KEY1);
	if (retval != ERROR_OK)
		return retval;

	retval = target_write_u32(target, STM32_FLASH_KEYR, KEY2);
	if (retval != ERROR_OK)
		return retval;

	/* unlock option flash registers */
	retval = target_write_u32(target, STM32_FLASH_OPTKEYR, KEY1);
	if (retval != ERROR_OK)
		return retval;
	retval = target_write_u32(target, STM32_FLASH_OPTKEYR, KEY2);
	if (retval != ERROR_OK)
		return retval;

	/* erase option bytes */
	retval = target_write_u32(target, STM32_FLASH_CR, FLASH_OPTER | FLASH_OPTWRE);
	if (retval != ERROR_OK)
		return retval;
	retval = target_write_u32(target, STM32_FLASH_CR, FLASH_OPTER | FLASH_STRT | FLASH_OPTWRE);
	if (retval != ERROR_OK)
		return retval;

	retval = stm32x_wait_status_busy(bank, 10);
	if (retval != ERROR_OK)
		return retval;

	/* clear readout protection and complementary option bytes
	 * this will also force a device unlock if set */
	stm32x_info->option_bytes.RDP = 0x5AA5;

	return ERROR_OK;
}
// stm32x specific function to reprogram the option byte. On the ST chip, this 
//	prevents reprogramming. Other chips will have other methods of doing this.
static int stm32x_write_options(struct flash_bank *bank)
{
	struct stm32x_flash_bank *stm32x_info = NULL;
	struct target *target = bank->target;

	stm32x_info = bank->driver_priv;

	/* unlock flash registers */
	int retval = target_write_u32(target, STM32_FLASH_KEYR, KEY1);
	if (retval != ERROR_OK)
		return retval;
	retval = target_write_u32(target, STM32_FLASH_KEYR, KEY2);
	if (retval != ERROR_OK)
		return retval;

	/* unlock option flash registers */
	retval = target_write_u32(target, STM32_FLASH_OPTKEYR, KEY1);
	if (retval != ERROR_OK)
		return retval;
	retval = target_write_u32(target, STM32_FLASH_OPTKEYR, KEY2);
	if (retval != ERROR_OK)
		return retval;

	/* program option bytes */
	retval = target_write_u32(target, STM32_FLASH_CR, FLASH_OPTPG | FLASH_OPTWRE);
	if (retval != ERROR_OK)
		return retval;

	/* write user option byte */
	retval = target_write_u16(target, STM32_OB_USER, stm32x_info->option_bytes.user_options);
	if (retval != ERROR_OK)
		return retval;

	retval = stm32x_wait_status_busy(bank, 10);
	if (retval != ERROR_OK)
		return retval;

	/* write protection byte 1 */
	retval = target_write_u16(target, STM32_OB_WRP0, stm32x_info->option_bytes.protection[0]);
	if (retval != ERROR_OK)
		return retval;

	retval = stm32x_wait_status_busy(bank, 10);
	if (retval != ERROR_OK)
		return retval;

	/* write protection byte 2 */
	retval = target_write_u16(target, STM32_OB_WRP1, stm32x_info->option_bytes.protection[1]);
	if (retval != ERROR_OK)
		return retval;

	retval = stm32x_wait_status_busy(bank, 10);
	if (retval != ERROR_OK)
		return retval;

	/* write protection byte 3 */
	retval = target_write_u16(target, STM32_OB_WRP2, stm32x_info->option_bytes.protection[2]);
	if (retval != ERROR_OK)
		return retval;

	retval = stm32x_wait_status_busy(bank, 10);
	if (retval != ERROR_OK)
		return retval;

	/* write protection byte 4 */
	retval = target_write_u16(target, STM32_OB_WRP3, stm32x_info->option_bytes.protection[3]);
	if (retval != ERROR_OK)
		return retval;

	retval = stm32x_wait_status_busy(bank, 10);
	if (retval != ERROR_OK)
		return retval;

	/* write readout protection bit */
	retval = target_write_u16(target, STM32_OB_RDP, stm32x_info->option_bytes.RDP);
	if (retval != ERROR_OK)
		return retval;

	retval = stm32x_wait_status_busy(bank, 10);
	if (retval != ERROR_OK)
		return retval;

	retval = target_write_u32(target, STM32_FLASH_CR, FLASH_LOCK);
	if (retval != ERROR_OK)
		return retval;

	return ERROR_OK;
}
/////////////////////////////////////////////////////////////////////////////
// Here begins the standard command set. These commands will be the main
//  interface to the part and are the heart of the flash driver. There will be
//  an entry for each of these in the flash_driver structure at the end of
//  this file. Reference:
// http://openocd.berlios.de/doc/doxygen/html/structflash__driver.html#aa4fda4f0ea001af0b75d7b9ca879b0d0
//
// It is required that these functions (described in the flash_driver structure)
//  exist and be entered int the flash_driver structure. Weird segmentation faults
//  may result otherwise (guess how I know).
//
// protect_check is the first of these functions, although they can be defined
//  in any order. protect_check checks each protection region in a specified bank
//  to see if it is protected in hardware. An appropriate entry is made in the 
//  sectors array in the bank structure.
// The algorithm for checking protection is specific to each device and
//  must be custom. That's why it's in the flash_driver!. Indeed, some devices
//  might not have this sort of protection at all. As mentioned above, the function
//  still must exist, even if it just returns.
static int stm32x_protect_check(struct flash_bank *bank)
{
	// Note the method of obtaining the target name from the bank structure.
	//  the target name is used in calls that read and write specific registers.
	struct target *target = bank->target;

	// This is the standard method of obtaining the pointer to the private
	//  data structure so that the private data can be accessed.
	struct stm32x_flash_bank *stm32x_info = bank->driver_priv;

	uint32_t protection;
	int i, s;
	int num_bits;
	int set;

	if (target->state != TARGET_HALTED)
	{
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	// stm32x specific function call
	int retval = stm32x_check_operation_supported(bank);
	if (ERROR_OK != retval)
		return retval;

	// The target_read_u32 is very commonly used to read specific registers
	//  for a device. The error code should always be checked.
	/* medium density - each bit refers to a 4bank protection
	 * high density - each bit refers to a 2bank protection */
	retval = target_read_u32(target, STM32_FLASH_WRPR, &protection);
	if (retval != ERROR_OK)
		return retval;

	/* medium density - each protection bit is for 4 * 1K pages
	 * high density - each protection bit is for 2 * 2K pages */
	num_bits = (bank->num_sectors / stm32x_info->ppage_size);

	if (stm32x_info->ppage_size == 2)
	{
		/* high density flash/connectivity line protection */

		set = 1;

		if (protection & (1 << 31))
			set = 0;

		/* bit 31 controls sector 62 - 255 protection for high density
		 * bit 31 controls sector 62 - 127 protection for connectivity line */
		for (s = 62; s < bank->num_sectors; s++)
		{
			bank->sectors[s].is_protected = set;
		}

		if (bank->num_sectors > 61)
			num_bits = 31;

		for (i = 0; i < num_bits; i++)
		{
			set = 1;

			if (protection & (1 << i))
				set = 0;

			for (s = 0; s < stm32x_info->ppage_size; s++)
				bank->sectors[(i * stm32x_info->ppage_size) + s].is_protected = set;
		}
	}
	else
	{
		/* low/medium density flash protection */
		for (i = 0; i < num_bits; i++)
		{
			set = 1;

			if (protection & (1 << i))
				set = 0;

			for (s = 0; s < stm32x_info->ppage_size; s++)
				bank->sectors[(i * stm32x_info->ppage_size) + s].is_protected = set;
		}
	}

	return ERROR_OK;
}
// erase is another standard function. The algorithm is of course custom to each device.
//  Getting a working algoritm for a new chip can take some time and experimentation
//  depending on the quality and completeness of the documentation.
static int stm32x_erase(struct flash_bank *bank, int first, int last)
{
	struct target *target = bank->target;
	int i;

	if (bank->target->state != TARGET_HALTED)
	{
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if ((first == 0) && (last == (bank->num_sectors - 1)))
	{
		return stm32x_mass_erase(bank);
	}

	/* unlock flash registers */
	int retval = target_write_u32(target, stm32x_get_flash_reg(bank, STM32_FLASH_KEYR), KEY1);
	if (retval != ERROR_OK)
		return retval;
	retval = target_write_u32(target, stm32x_get_flash_reg(bank, STM32_FLASH_KEYR), KEY2);
	if (retval != ERROR_OK)
		return retval;

	for (i = first; i <= last; i++)
	{
		retval = target_write_u32(target, stm32x_get_flash_reg(bank, STM32_FLASH_CR), FLASH_PER);
		if (retval != ERROR_OK)
			return retval;
		retval = target_write_u32(target, stm32x_get_flash_reg(bank, STM32_FLASH_AR),
				bank->base + bank->sectors[i].offset);
		if (retval != ERROR_OK)
			return retval;
		retval = target_write_u32(target,
				stm32x_get_flash_reg(bank, STM32_FLASH_CR), FLASH_PER | FLASH_STRT);
		if (retval != ERROR_OK)
			return retval;

		retval = stm32x_wait_status_busy(bank, 100);
		if (retval != ERROR_OK)
			return retval;

		bank->sectors[i].is_erased = 1;
	}

	retval = target_write_u32(target, stm32x_get_flash_reg(bank, STM32_FLASH_CR), FLASH_LOCK);
	if (retval != ERROR_OK)
		return retval;

	return ERROR_OK;
}

// The function to protect segments of memory. Some chips may not have this capability.
//  But the function must exist!
static int stm32x_protect(struct flash_bank *bank, int set, int first, int last)
{
	struct stm32x_flash_bank *stm32x_info = NULL;
	struct target *target = bank->target;
	uint16_t prot_reg[4] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
	int i, reg, bit;
	int status;
	uint32_t protection;

	stm32x_info = bank->driver_priv;

	if (target->state != TARGET_HALTED)
	{
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	int retval = stm32x_check_operation_supported(bank);
	if (ERROR_OK != retval)
		return retval;

	if ((first % stm32x_info->ppage_size) != 0)
	{
		LOG_WARNING("aligned start protect sector to a %d sector boundary",
				stm32x_info->ppage_size);
		first = first - (first % stm32x_info->ppage_size);
	}
	if (((last + 1) % stm32x_info->ppage_size) != 0)
	{
		LOG_WARNING("aligned end protect sector to a %d sector boundary",
				stm32x_info->ppage_size);
		last++;
		last = last - (last % stm32x_info->ppage_size);
		last--;
	}

	/* medium density - each bit refers to a 4bank protection
	 * high density - each bit refers to a 2bank protection */
	retval = target_read_u32(target, STM32_FLASH_WRPR, &protection);
	if (retval != ERROR_OK)
		return retval;

	prot_reg[0] = (uint16_t)protection;
	prot_reg[1] = (uint16_t)(protection >> 8);
	prot_reg[2] = (uint16_t)(protection >> 16);
	prot_reg[3] = (uint16_t)(protection >> 24);

	if (stm32x_info->ppage_size == 2)
	{
		/* high density flash */

		/* bit 7 controls sector 62 - 255 protection */
		if (last > 61)
		{
			if (set)
				prot_reg[3] &= ~(1 << 7);
			else
				prot_reg[3] |= (1 << 7);
		}

		if (first > 61)
			first = 62;
		if (last > 61)
			last = 61;

		for (i = first; i <= last; i++)
		{
			reg = (i / stm32x_info->ppage_size) / 8;
			bit = (i / stm32x_info->ppage_size) - (reg * 8);

			if (set)
				prot_reg[reg] &= ~(1 << bit);
			else
				prot_reg[reg] |= (1 << bit);
		}
	}
	else
	{
		/* medium density flash */
		for (i = first; i <= last; i++)
		{
			reg = (i / stm32x_info->ppage_size) / 8;
			bit = (i / stm32x_info->ppage_size) - (reg * 8);

			if (set)
				prot_reg[reg] &= ~(1 << bit);
			else
				prot_reg[reg] |= (1 << bit);
		}
	}

	if ((status = stm32x_erase_options(bank)) != ERROR_OK)
		return status;

	stm32x_info->option_bytes.protection[0] = prot_reg[0];
	stm32x_info->option_bytes.protection[1] = prot_reg[1];
	stm32x_info->option_bytes.protection[2] = prot_reg[2];
	stm32x_info->option_bytes.protection[3] = prot_reg[3];

	return stm32x_write_options(bank);
}

// This is a helper function for the write function which follows.
//  Getting write right can also take a lot of time and playing
//  around with a new chip. 
// The key idea is that code to be programmed into the chip is placed
//  in a buffer in on-chip sram. A routine to do the programming is
//  loaded into on-chip sram as well. The controlling function starts the
//  programming code and keeps the buffer full. This is much faster than
//  trying to program each word individually. Studying this code carefully
//  is well worth while.
static int stm32x_write_block(struct flash_bank *bank, uint8_t *buffer,
		uint32_t offset, uint32_t count)
{
	struct stm32x_flash_bank *stm32x_info = bank->driver_priv;
	struct target *target = bank->target;
	uint32_t buffer_size = 16384;
	struct working_area *source;
	uint32_t address = bank->base + offset;
	struct reg_param reg_params[4];
	struct armv7m_algorithm armv7m_info;
	int retval = ERROR_OK;

	/* see contib/loaders/flash/stm32x.s for src */
	// This is a very nice piece of documentation. Not all drivers provide
	//  it. It's the source program for the assembly code that follows.

	static const uint8_t stm32x_flash_write_code[] = {
									/* #define STM32_FLASH_CR_OFFSET	0x10 */
									/* #define STM32_FLASH_SR_OFFSET	0x0C */
									/* write: */
		0x08, 0x4c,					/* ldr	r4, STM32_FLASH_BASE */
		0x1c, 0x44,					/* add	r4, r3 */
									/* write_half_word: */
		0x01, 0x23,					/* movs	r3, #0x01 */
		0x23, 0x61,					/* str	r3, [r4, #STM32_FLASH_CR_OFFSET] */
		0x30, 0xf8, 0x02, 0x3b,		/* ldrh	r3, [r0], #0x02 */
		0x21, 0xf8, 0x02, 0x3b,		/* strh	r3, [r1], #0x02 */
									/* busy: */
		0xe3, 0x68,					/* ldr	r3, [r4, #STM32_FLASH_SR_OFFSET] */
		0x13, 0xf0, 0x01, 0x0f,		/* tst	r3, #0x01 */
		0xfb, 0xd0,					/* beq	busy */
		0x13, 0xf0, 0x14, 0x0f,		/* tst	r3, #0x14 */
		0x01, 0xd1,					/* bne	exit */
		0x01, 0x3a,					/* subs	r2, r2, #0x01 */
		0xf0, 0xd1,					/* bne	write_half_word */
									/* exit: */
		0x00, 0xbe,					/* bkpt	#0x00 */
		0x00, 0x20, 0x02, 0x40,		/* STM32_FLASH_BASE: .word 0x40022000 */
	};

	/* flash write code */
	if (target_alloc_working_area(target, sizeof(stm32x_flash_write_code),
			&stm32x_info->write_algorithm) != ERROR_OK)
	{
		LOG_WARNING("no working area available, can't do block memory writes");
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	};

	if ((retval = target_write_buffer(target, stm32x_info->write_algorithm->address,
			sizeof(stm32x_flash_write_code),
			(uint8_t*)stm32x_flash_write_code)) != ERROR_OK)
		return retval;

	/* memory buffer */
	while (target_alloc_working_area_try(target, buffer_size, &source) != ERROR_OK)
	{
		buffer_size /= 2;
		if (buffer_size <= 256)
		{
			/* if we already allocated the writing code, but failed to get a
			 * buffer, free the algorithm */
			if (stm32x_info->write_algorithm)
				target_free_working_area(target, stm32x_info->write_algorithm);

			LOG_WARNING("no large enough working area available, can't do block memory writes");
			return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
		}
	};

	// I do not know exactly how the following code works. The effect seems to
	//  be to allow placing specific values in specific registers for the call to the
	//  assembly language routine that writes memory segments. A structure with an
	//  entry for each register seems to be created.

	armv7m_info.common_magic = ARMV7M_COMMON_MAGIC;
	armv7m_info.core_mode = ARMV7M_MODE_ANY;

	init_reg_param(&reg_params[0], "r0", 32, PARAM_OUT);
	init_reg_param(&reg_params[1], "r1", 32, PARAM_OUT);
	init_reg_param(&reg_params[2], "r2", 32, PARAM_OUT);
	init_reg_param(&reg_params[3], "r3", 32, PARAM_IN_OUT);

	// OK, I understand the code again.
	while (count > 0)
	{
		uint32_t thisrun_count = (count > (buffer_size / 2)) ?
				(buffer_size / 2) : count;

		if ((retval = target_write_buffer(target, source->address,
				thisrun_count * 2, buffer)) != ERROR_OK)
			break;

		buf_set_u32(reg_params[0].value, 0, 32, source->address);
		buf_set_u32(reg_params[1].value, 0, 32, address);
		buf_set_u32(reg_params[2].value, 0, 32, thisrun_count);
		buf_set_u32(reg_params[3].value, 0, 32, stm32x_info->register_offset);

		if ((retval = target_run_algorithm(target, 0, NULL, 4, reg_params,
				stm32x_info->write_algorithm->address,
				0,
				10000, &armv7m_info)) != ERROR_OK)
		{
			LOG_ERROR("error executing stm32x flash write algorithm");
			break;
		}

		if (buf_get_u32(reg_params[3].value, 0, 32) & FLASH_PGERR)
		{
			LOG_ERROR("flash memory not erased before writing");
			/* Clear but report errors */
			target_write_u32(target, STM32_FLASH_SR, FLASH_PGERR);
			retval = ERROR_FAIL;
			break;
		}

		if (buf_get_u32(reg_params[3].value, 0, 32) & FLASH_WRPRTERR)
		{
			LOG_ERROR("flash memory write protected");
			/* Clear but report errors */
			target_write_u32(target, STM32_FLASH_SR, FLASH_WRPRTERR);
			retval = ERROR_FAIL;
			break;
		}

		buffer += thisrun_count * 2;
		address += thisrun_count * 2;
		count -= thisrun_count;
	}

	target_free_working_area(target, source);
	target_free_working_area(target, stm32x_info->write_algorithm);

	destroy_reg_param(&reg_params[0]);
	destroy_reg_param(&reg_params[1]);
	destroy_reg_param(&reg_params[2]);
	destroy_reg_param(&reg_params[3]);

	return retval;
}

// This is the main write routine. It uses the helper function above.
static int stm32x_write(struct flash_bank *bank, uint8_t *buffer,
		uint32_t offset, uint32_t count)
{
	struct target *target = bank->target;
	uint32_t words_remaining = (count / 2);
	uint32_t bytes_remaining = (count & 0x00000001);
	uint32_t address = bank->base + offset;
	uint32_t bytes_written = 0;
	int retval;

	if (bank->target->state != TARGET_HALTED)
	{
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (offset & 0x1)
	{
		LOG_WARNING("offset 0x%" PRIx32 " breaks required 2-byte alignment", offset);
		return ERROR_FLASH_DST_BREAKS_ALIGNMENT;
	}

	/* unlock flash registers */
	retval = target_write_u32(target, stm32x_get_flash_reg(bank, STM32_FLASH_KEYR), KEY1);
	if (retval != ERROR_OK)
		return retval;
	retval = target_write_u32(target, stm32x_get_flash_reg(bank, STM32_FLASH_KEYR), KEY2);
	if (retval != ERROR_OK)
		return retval;

	/* multiple half words (2-byte) to be programmed? */
	if (words_remaining > 0)
	{
		/* try using a block write */
		if ((retval = stm32x_write_block(bank, buffer, offset, words_remaining)) != ERROR_OK)
		{
			if (retval == ERROR_TARGET_RESOURCE_NOT_AVAILABLE)
			{
				/* if block write failed (no sufficient working area),
				 * we use normal (slow) single dword accesses */
				LOG_WARNING("couldn't use block writes, falling back to single memory accesses");
			}
		}
		else
		{
			buffer += words_remaining * 2;
			address += words_remaining * 2;
			words_remaining = 0;
		}
	}

	if ((retval != ERROR_OK) && (retval != ERROR_TARGET_RESOURCE_NOT_AVAILABLE))
		return retval;

	while (words_remaining > 0)
	{
		uint16_t value;
		memcpy(&value, buffer + bytes_written, sizeof(uint16_t));

		retval = target_write_u32(target, stm32x_get_flash_reg(bank, STM32_FLASH_CR), FLASH_PG);
		if (retval != ERROR_OK)
			return retval;
		retval = target_write_u16(target, address, value);
		if (retval != ERROR_OK)
			return retval;

		retval = stm32x_wait_status_busy(bank, 5);
		if (retval != ERROR_OK)
			return retval;

		bytes_written += 2;
		words_remaining--;
		address += 2;
	}

	if (bytes_remaining)
	{
		uint16_t value = 0xffff;
		memcpy(&value, buffer + bytes_written, bytes_remaining);

		retval = target_write_u32(target, stm32x_get_flash_reg(bank, STM32_FLASH_CR), FLASH_PG);
		if (retval != ERROR_OK)
			return retval;
		retval = target_write_u16(target, address, value);
		if (retval != ERROR_OK)
			return retval;

		retval = stm32x_wait_status_busy(bank, 5);
		if (retval != ERROR_OK)
			return retval;
	}

	return target_write_u32(target, STM32_FLASH_CR, FLASH_LOCK);
}

// The probe routine. If possible, an appropriate register on the chip should be
//  read to verify the type of chip. Various flavors and sizes of chips of the same
//  general type can be accomodated this way. It also is good to verify that the 
//  chip is the kind that the configuration file thought it was.
static int stm32x_probe(struct flash_bank *bank)
{
	struct target *target = bank->target;
	struct stm32x_flash_bank *stm32x_info = bank->driver_priv;
	int i;
	uint16_t num_pages;
	uint32_t device_id;
	int page_size;
	uint32_t base_address = 0x08000000;

	stm32x_info->probed = 0;
	stm32x_info->register_offset = FLASH_OFFSET_B0;

	/* read stm32 device id register */
	int retval = target_read_u32(target, 0xE0042000, &device_id);
	if (retval != ERROR_OK)
		return retval;
	LOG_INFO("device id = 0x%08" PRIx32 "", device_id);

	/* get flash size from target. */
	retval = target_read_u16(target, 0x1FFFF7E0, &num_pages);
	if (retval != ERROR_OK)
	{
		LOG_WARNING("failed reading flash size, default to max target family");
		/* failed reading flash size, default to max target family */
		num_pages = 0xffff;
	}

	if ((device_id & 0x7ff) == 0x410)
	{
		/* medium density - we have 1k pages
		 * 4 pages for a protection area */
		page_size = 1024;
		stm32x_info->ppage_size = 4;

		/* check for early silicon */
		if (num_pages == 0xffff)
		{
			/* number of sectors incorrect on revA */
			LOG_WARNING("STM32 flash size failed, probe inaccurate - assuming 128k flash");
			num_pages = 128;
		}
	}
	else if ((device_id & 0x7ff) == 0x412)
	{
		/* low density - we have 1k pages
		 * 4 pages for a protection area */
		page_size = 1024;
		stm32x_info->ppage_size = 4;

		/* check for early silicon */
		if (num_pages == 0xffff)
		{
			/* number of sectors incorrect on revA */
			LOG_WARNING("STM32 flash size failed, probe inaccurate - assuming 32k flash");
			num_pages = 32;
		}
	}
	else if ((device_id & 0x7ff) == 0x414)
	{
		/* high density - we have 2k pages
		 * 2 pages for a protection area */
		page_size = 2048;
		stm32x_info->ppage_size = 2;

		/* check for early silicon */
		if (num_pages == 0xffff)
		{
			/* number of sectors incorrect on revZ */
			LOG_WARNING("STM32 flash size failed, probe inaccurate - assuming 512k flash");
			num_pages = 512;
		}
	}
	else if ((device_id & 0x7ff) == 0x418)
	{
		/* connectivity line density - we have 2k pages
		 * 2 pages for a protection area */
		page_size = 2048;
		stm32x_info->ppage_size = 2;

		/* check for early silicon */
		if (num_pages == 0xffff)
		{
			/* number of sectors incorrect on revZ */
			LOG_WARNING("STM32 flash size failed, probe inaccurate - assuming 256k flash");
			num_pages = 256;
		}
	}
	else if ((device_id & 0x7ff) == 0x420)
	{
		/* value line density - we have 1k pages
		 * 4 pages for a protection area */
		page_size = 1024;
		stm32x_info->ppage_size = 4;

		/* check for early silicon */
		if (num_pages == 0xffff)
		{
			/* number of sectors may be incorrrect on early silicon */
			LOG_WARNING("STM32 flash size failed, probe inaccurate - assuming 128k flash");
			num_pages = 128;
		}
	}
	else if ((device_id & 0x7ff) == 0x430)
	{
		/* xl line density - we have 2k pages
		 * 2 pages for a protection area */
		page_size = 2048;
		stm32x_info->ppage_size = 2;
		stm32x_info->has_dual_banks = true;

		/* check for early silicon */
		if (num_pages == 0xffff)
		{
			/* number of sectors may be incorrrect on early silicon */
			LOG_WARNING("STM32 flash size failed, probe inaccurate - assuming 1024k flash");
			num_pages = 1024;
		}

		/* split reported size into matching bank */
		if (bank->base != 0x08080000)
		{
			/* bank 0 will be fixed 512k */
			num_pages = 512;
		}
		else
		{
			num_pages -= 512;
			/* bank1 also uses a register offset */
			stm32x_info->register_offset = FLASH_OFFSET_B1;
			base_address = 0x08080000;
		}
	}
	else
	{
		LOG_WARNING("Cannot identify target as a STM32 family.");
		return ERROR_FAIL;
	}

	LOG_INFO("flash size = %dkbytes", num_pages);

	/* calculate numbers of pages */
	num_pages /= (page_size / 1024);

	if (bank->sectors)
	{
		free(bank->sectors);
		bank->sectors = NULL;
	}

	bank->base = base_address;
	bank->size = (num_pages * page_size);
	bank->num_sectors = num_pages;
	bank->sectors = malloc(sizeof(struct flash_sector) * num_pages);

	for (i = 0; i < num_pages; i++)
	{
		bank->sectors[i].offset = i * page_size;
		bank->sectors[i].size = page_size;
		bank->sectors[i].is_erased = -1;
		bank->sectors[i].is_protected = 1;
	}

	// Here the probed flag is set in the bank private data.
	//  This saves having to do the probe all the time since
	//  other functions rely on this data existing.
	stm32x_info->probed = 1;

	return ERROR_OK;
}

// It's easy to see that auto_probe only probes if the "probed" bit isn't set.
//  This is the function that other functions use to make sure probe data has
//  been gathered.
static int stm32x_auto_probe(struct flash_bank *bank)
{
	struct stm32x_flash_bank *stm32x_info = bank->driver_priv;
	if (stm32x_info->probed)
		return ERROR_OK;
	return stm32x_probe(bank);
}

// looks like this was going to be used, but wasn't?
#if 0
COMMAND_HANDLER(stm32x_handle_part_id_command)
{
	return ERROR_OK;
}
#endif


// The info function for the chip. This appears to be redundant to the probe
//  function, but there are differences. It might be possible to combine them, 
//  but why?
static int get_stm32x_info(struct flash_bank *bank, char *buf, int buf_size)
{
	struct target *target = bank->target;
	uint32_t device_id;
	int printed;

	/* read stm32 device id register */
	int retval = target_read_u32(target, 0xE0042000, &device_id);
	if (retval != ERROR_OK)
		return retval;

	if ((device_id & 0x7ff) == 0x410)
	{
		printed = snprintf(buf, buf_size, "stm32x (Medium Density) - Rev: ");
		buf += printed;
		buf_size -= printed;

		switch (device_id >> 16)
		{
			case 0x0000:
				snprintf(buf, buf_size, "A");
				break;

			case 0x2000:
				snprintf(buf, buf_size, "B");
				break;

			case 0x2001:
				snprintf(buf, buf_size, "Z");
				break;

			case 0x2003:
				snprintf(buf, buf_size, "Y");
				break;

			default:
				snprintf(buf, buf_size, "unknown");
				break;
		}
	}
	else if ((device_id & 0x7ff) == 0x412)
	{
		printed = snprintf(buf, buf_size, "stm32x (Low Density) - Rev: ");
		buf += printed;
		buf_size -= printed;

		switch (device_id >> 16)
		{
			case 0x1000:
				snprintf(buf, buf_size, "A");
				break;

			default:
				snprintf(buf, buf_size, "unknown");
				break;
		}
	}
	else if ((device_id & 0x7ff) == 0x414)
	{
		printed = snprintf(buf, buf_size, "stm32x (High Density) - Rev: ");
		buf += printed;
		buf_size -= printed;

		switch (device_id >> 16)
		{
			case 0x1000:
				snprintf(buf, buf_size, "A");
				break;

			case 0x1001:
				snprintf(buf, buf_size, "Z");
				break;

			default:
				snprintf(buf, buf_size, "unknown");
				break;
		}
	}
	else if ((device_id & 0x7ff) == 0x418)
	{
		printed = snprintf(buf, buf_size, "stm32x (Connectivity) - Rev: ");
		buf += printed;
		buf_size -= printed;

		switch (device_id >> 16)
		{
			case 0x1000:
				snprintf(buf, buf_size, "A");
				break;

			case 0x1001:
				snprintf(buf, buf_size, "Z");
				break;

			default:
				snprintf(buf, buf_size, "unknown");
				break;
		}
	}
	else if ((device_id & 0x7ff) == 0x420)
	{
		printed = snprintf(buf, buf_size, "stm32x (Value) - Rev: ");
		buf += printed;
		buf_size -= printed;

		switch (device_id >> 16)
		{
			case 0x1000:
				snprintf(buf, buf_size, "A");
				break;

			case 0x1001:
				snprintf(buf, buf_size, "Z");
				break;

			default:
				snprintf(buf, buf_size, "unknown");
				break;
		}
	}
	else if ((device_id & 0x7ff) == 0x430)
	{
		printed = snprintf(buf, buf_size, "stm32x (XL) - Rev: ");
		buf += printed;
		buf_size -= printed;

		switch (device_id >> 16)
		{
			case 0x1000:
				snprintf(buf, buf_size, "A");
				break;

			default:
				snprintf(buf, buf_size, "unknown");
				break;
		}
	}
	else
	{
		snprintf(buf, buf_size, "Cannot identify target as a stm32x\n");
		return ERROR_FAIL;
	}

	return ERROR_OK;
}

///////////////////////////////////////////////////////////////////////////////////////////////
// Special commands for the stm32x start here. Most of these use the option byte manipulation
//  routines that were defined above. Various combinations of these functions are used to 
//  implement the functions a user might want to do.

COMMAND_HANDLER(stm32x_handle_lock_command)
{
	struct target *target = NULL;
	struct stm32x_flash_bank *stm32x_info = NULL;

	if (CMD_ARGC < 1)
	{
		command_print(CMD_CTX, "stm32x lock <bank>");
		return ERROR_OK;
	}

	struct flash_bank *bank;
	int retval = CALL_COMMAND_HANDLER(flash_command_get_bank, 0, &bank);
	if (ERROR_OK != retval)
		return retval;

	stm32x_info = bank->driver_priv;

	target = bank->target;

	if (target->state != TARGET_HALTED)
	{
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	retval = stm32x_check_operation_supported(bank);
	if (ERROR_OK != retval)
		return retval;

	if (stm32x_erase_options(bank) != ERROR_OK)
	{
		command_print(CMD_CTX, "stm32x failed to erase options");
		return ERROR_OK;
	}

	/* set readout protection */
	stm32x_info->option_bytes.RDP = 0;

	if (stm32x_write_options(bank) != ERROR_OK)
	{
		command_print(CMD_CTX, "stm32x failed to lock device");
		return ERROR_OK;
	}

	command_print(CMD_CTX, "stm32x locked");

	return ERROR_OK;
}

COMMAND_HANDLER(stm32x_handle_unlock_command)
{
	struct target *target = NULL;
	struct stm32x_flash_bank *stm32x_info = NULL;

	if (CMD_ARGC < 1)
	{
		command_print(CMD_CTX, "stm32x unlock <bank>");
		return ERROR_OK;
	}

	struct flash_bank *bank;
	int retval = CALL_COMMAND_HANDLER(flash_command_get_bank, 0, &bank);
	if (ERROR_OK != retval)
		return retval;

	stm32x_info = bank->driver_priv;

	target = bank->target;

	if (target->state != TARGET_HALTED)
	{
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	retval = stm32x_check_operation_supported(bank);
	if (ERROR_OK != retval)
		return retval;

	if (stm32x_erase_options(bank) != ERROR_OK)
	{
		command_print(CMD_CTX, "stm32x failed to unlock device");
		return ERROR_OK;
	}

	if (stm32x_write_options(bank) != ERROR_OK)
	{
		command_print(CMD_CTX, "stm32x failed to lock device");
		return ERROR_OK;
	}

	command_print(CMD_CTX, "stm32x unlocked.\n"
			"INFO: a reset or power cycle is required "
			"for the new settings to take effect.");

	return ERROR_OK;
}

COMMAND_HANDLER(stm32x_handle_options_read_command)
{
	uint32_t optionbyte;
	struct target *target = NULL;
	struct stm32x_flash_bank *stm32x_info = NULL;

	if (CMD_ARGC < 1)
	{
		command_print(CMD_CTX, "stm32x options_read <bank>");
		return ERROR_OK;
	}

	struct flash_bank *bank;
	int retval = CALL_COMMAND_HANDLER(flash_command_get_bank, 0, &bank);
	if (ERROR_OK != retval)
		return retval;

	stm32x_info = bank->driver_priv;

	target = bank->target;

	if (target->state != TARGET_HALTED)
	{
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	retval = stm32x_check_operation_supported(bank);
	if (ERROR_OK != retval)
		return retval;

	retval = target_read_u32(target, STM32_FLASH_OBR, &optionbyte);
	if (retval != ERROR_OK)
		return retval;
	command_print(CMD_CTX, "Option Byte: 0x%" PRIx32 "", optionbyte);

	if (buf_get_u32((uint8_t*)&optionbyte, OPT_ERROR, 1))
		command_print(CMD_CTX, "Option Byte Complement Error");

	if (buf_get_u32((uint8_t*)&optionbyte, OPT_READOUT, 1))
		command_print(CMD_CTX, "Readout Protection On");
	else
		command_print(CMD_CTX, "Readout Protection Off");

	if (buf_get_u32((uint8_t*)&optionbyte, OPT_RDWDGSW, 1))
		command_print(CMD_CTX, "Software Watchdog");
	else
		command_print(CMD_CTX, "Hardware Watchdog");

	if (buf_get_u32((uint8_t*)&optionbyte, OPT_RDRSTSTOP, 1))
		command_print(CMD_CTX, "Stop: No reset generated");
	else
		command_print(CMD_CTX, "Stop: Reset generated");

	if (buf_get_u32((uint8_t*)&optionbyte, OPT_RDRSTSTDBY, 1))
		command_print(CMD_CTX, "Standby: No reset generated");
	else
		command_print(CMD_CTX, "Standby: Reset generated");

	if (stm32x_info->has_dual_banks)
	{
		if (buf_get_u32((uint8_t*)&optionbyte, OPT_BFB2, 1))
			command_print(CMD_CTX, "Boot: Bank 0");
		else
			command_print(CMD_CTX, "Boot: Bank 1");
	}

	return ERROR_OK;
}

COMMAND_HANDLER(stm32x_handle_options_write_command)
{
	struct target *target = NULL;
	struct stm32x_flash_bank *stm32x_info = NULL;
	uint16_t optionbyte = 0xF8;

	if (CMD_ARGC < 4)
	{
		command_print(CMD_CTX, "stm32x options_write <bank> <SWWDG | HWWDG> "
				"<RSTSTNDBY | NORSTSTNDBY> <RSTSTOP | NORSTSTOP> <BOOT0 | BOOT1>");
		return ERROR_OK;
	}

	struct flash_bank *bank;
	int retval = CALL_COMMAND_HANDLER(flash_command_get_bank, 0, &bank);
	if (ERROR_OK != retval)
		return retval;

	stm32x_info = bank->driver_priv;

	target = bank->target;

	if (target->state != TARGET_HALTED)
	{
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	retval = stm32x_check_operation_supported(bank);
	if (ERROR_OK != retval)
		return retval;

	/* REVISIT: ignores some options which we will display...
	 * and doesn't insist on the specified syntax.
	 */

	/* OPT_RDWDGSW */
	if (strcmp(CMD_ARGV[1], "SWWDG") == 0)
	{
		optionbyte |= (1 << 0);
	}
	else	/* REVISIT must be "HWWDG" then ... */
	{
		optionbyte &= ~(1 << 0);
	}

	/* OPT_RDRSTSTOP */
	if (strcmp(CMD_ARGV[2], "NORSTSTOP") == 0)
	{
		optionbyte |= (1 << 1);
	}
	else	/* REVISIT must be "RSTSTNDBY" then ... */
	{
		optionbyte &= ~(1 << 1);
	}

	/* OPT_RDRSTSTDBY */
	if (strcmp(CMD_ARGV[3], "NORSTSTNDBY") == 0)
	{
		optionbyte |= (1 << 2);
	}
	else	/* REVISIT must be "RSTSTOP" then ... */
	{
		optionbyte &= ~(1 << 2);
	}

	if (CMD_ARGC > 4 && stm32x_info->has_dual_banks)
	{
		/* OPT_BFB2 */
		if (strcmp(CMD_ARGV[4], "BOOT0") == 0)
		{
			optionbyte |= (1 << 3);
		}
		else
		{
			optionbyte &= ~(1 << 3);
		}
	}

	if (stm32x_erase_options(bank) != ERROR_OK)
	{
		command_print(CMD_CTX, "stm32x failed to erase options");
		return ERROR_OK;
	}

	stm32x_info->option_bytes.user_options = optionbyte;

	if (stm32x_write_options(bank) != ERROR_OK)
	{
		command_print(CMD_CTX, "stm32x failed to write options");
		return ERROR_OK;
	}

	command_print(CMD_CTX, "stm32x write options complete.\n"
				"INFO: a reset or power cycle is required "
				"for the new settings to take effect.");

	return ERROR_OK;
}

// Implementation of the mass erase command for the stm32x.
//  Invoked as a command by the next function.
static int stm32x_mass_erase(struct flash_bank *bank)
{
	struct target *target = bank->target;

	if (target->state != TARGET_HALTED)
	{
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	/* unlock option flash registers */
	int retval = target_write_u32(target, stm32x_get_flash_reg(bank, STM32_FLASH_KEYR), KEY1);
	if (retval != ERROR_OK)
		return retval;
	retval = target_write_u32(target, stm32x_get_flash_reg(bank, STM32_FLASH_KEYR), KEY2);
	if (retval != ERROR_OK)
		return retval;

	/* mass erase flash memory */
	retval = target_write_u32(target, stm32x_get_flash_reg(bank, STM32_FLASH_CR), FLASH_MER);
	if (retval != ERROR_OK)
		return retval;
	retval = target_write_u32(target, stm32x_get_flash_reg(bank, STM32_FLASH_CR), FLASH_MER | FLASH_STRT);
	if (retval != ERROR_OK)
		return retval;

	retval = stm32x_wait_status_busy(bank, 100);
	if (retval != ERROR_OK)
		return retval;

	retval = target_write_u32(target, stm32x_get_flash_reg(bank, STM32_FLASH_CR), FLASH_LOCK);
	if (retval != ERROR_OK)
		return retval;

	return ERROR_OK;
}

// Supports the mass erase command for the stm32x. Uses the function above to do the work.
//  This function must be placed in the stm32x_exec_command_handlers structure to be 
//  registered and made accessible to the user.
COMMAND_HANDLER(stm32x_handle_mass_erase_command)
{
	int i;

	if (CMD_ARGC < 1)
	{
		command_print(CMD_CTX, "stm32x mass_erase <bank>");
		return ERROR_OK;
	}

	struct flash_bank *bank;
	int retval = CALL_COMMAND_HANDLER(flash_command_get_bank, 0, &bank);
	if (ERROR_OK != retval)
		return retval;

	retval = stm32x_mass_erase(bank);
	if (retval == ERROR_OK)
	{
		/* set all sectors as erased */
		for (i = 0; i < bank->num_sectors; i++)
		{
			bank->sectors[i].is_erased = 1;
		}

		command_print(CMD_CTX, "stm32x mass erase complete");
	}
	else
	{
		command_print(CMD_CTX, "stm32x mass erase failed");
	}

	return retval;
}
// This structure supports registering additional device-specific commands 
//  beyond the basic, required set. This structure enumerates the commands 
//  and associates their names. It is then used by the following structure
//  to actually register the commands and make them available to the user.
// The stm32x additional commands are added here and will be available when
//  the flash driver is used.
static const struct command_registration stm32x_exec_command_handlers[] = {
	{
		.name = "lock",
		.handler = stm32x_handle_lock_command,
		.mode = COMMAND_EXEC,
		.usage = "bank_id",
		.help = "Lock entire flash device.",
	},
	{
		.name = "unlock",
		.handler = stm32x_handle_unlock_command,
		.mode = COMMAND_EXEC,
		.usage = "bank_id",
		.help = "Unlock entire protected flash device.",
	},
	{
		.name = "mass_erase",
		.handler = stm32x_handle_mass_erase_command,
		.mode = COMMAND_EXEC,
		.usage = "bank_id",
		.help = "Erase entire flash device.",
	},
	{
		.name = "options_read",
		.handler = stm32x_handle_options_read_command,
		.mode = COMMAND_EXEC,
		.usage = "bank_id",
		.help = "Read and display device option byte.",
	},
	{
		.name = "options_write",
		.handler = stm32x_handle_options_write_command,
		.mode = COMMAND_EXEC,
		.usage = "bank_id ('SWWDG'|'HWWDG') "
			"('RSTSTNDBY'|'NORSTSTNDBY') "
			"('RSTSTOP'|'NORSTSTOP')",
		.help = "Replace bits in device option byte.",
	},
	COMMAND_REGISTRATION_DONE
};

// This structure provides the link to the additional commands for a 
//  specific device. The individual commands are collected in the structure
//  above. This linkage perhaps a bit convoluted, but it is not difficult
//  if you walk through it in steps.
static const struct command_registration stm32x_command_handlers[] = {
	{
		.name = "stm32x",
		.mode = COMMAND_ANY,
		.help = "stm32x flash command group",
		.chain = stm32x_exec_command_handlers,
	},
	COMMAND_REGISTRATION_DONE
};
// This is a key structure for your flash driver. You should give it a good
//  identifying name. E.g., if your driver is myCoolPart, then the struct name might be
//  myCoolPart_flash. The flash_driver structure is described here: 
//  http://openocd.berlios.de/doc/doxygen/html/structflash__driver.html#_details 
// The description includes details of each of the required functions, how they 
//  are called, and what they need to do. If this location changes in the future, 
//  doing "make doxygen" should create the documentation locally.
// Most of the fields are pretty self-explanatory: just an equate to the name of
//  the functions already written. It is perfectly acceptable to point to the
//  default functions when these do the job. Three fields need additional explanation.
// The .name field is the name this driver will be known by. Following the example,
//  name the driver "myCoolPart".
// The .commands field is used to add commands to the basic set. These have been collected
//  in a structure whose name was included in a second structure as described above. The name
//  of the second structure is placed in this field as shown. If no commands are to be added,
//  then this become .commands = null.
// The .flash_bank_command entry points to the function of type FLASH_BANK_COMMAND_HANDLER 
//  defined near the top of this file. 
//
// The documentation says "After that, the instance must be registered in flash.c, 
//  so it can be used by the driver lookup system." There is no file named flash.c
//  (in the nor subdirectory, at least). The registration happens in drivers.c and is
//  described in the documentation provided in my project (of which this file is part.)
struct flash_driver stm32x_flash = {
	.name = "stm32x",
	.commands = stm32x_command_handlers,
	.flash_bank_command = stm32x_flash_bank_command,
	.erase = stm32x_erase,
	.protect = stm32x_protect,
	.write = stm32x_write,
	.read = default_flash_read,
	.probe = stm32x_probe,
	.auto_probe = stm32x_auto_probe,
	.erase_check = default_flash_mem_blank_check,
	.protect_check = stm32x_protect_check,
	.info = get_stm32x_info,
};
