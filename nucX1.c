/***************************************************************************
 *   Copyright (C) 2011 by James K. Larson                                 *
 *  jlarson@pacifier.com                                                    *
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

// This flash driver takes it's inspiration and structure from the stm32x
//	flash driver in the OpenOCD package.
// 7/16/11 -- Initial dummy version works - now add functions.  -jkl

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "imp.h"

/* nucX1 register locations */
#define NUCX1_SYS_BASE		0x50000000
#define NUCX1_SYS_WRPROT	0x50000100

#define NUCX1_SYSCLK_BASE	0x50000200
#define NUCX1_SYSCLK_PWRCON	0x50000200
#define NUCX1_SYSCLK_CLKSEL0	0x50000210
#define NUCX1_SYSCLK_CLKDIV	0x50000218
#define NUCX1_SYSCLK_AHBCLK	0x50000204

#define NUCX1_FLASH_BASE	0x5000C000
#define NUCX1_FLASH_ISPCON	0x5000C000
#define NUCX1_FLASH_ISPCMD	0x5000C00C
#define NUCX1_FLASH_ISPADR	0x5000C004
//#define NUCX1_FLASH_ISPDAT	0x5000C00?
#define NUCX1_FLASH_ISPTRG	0x5000C010


/* Command register bits */

#define PWRCON_OSC22M		(1 << 2)
#define PWRCON_XTL12M		(1 << 0)

#define AHBCLK_ISP_EN		(1 << 2)

#define ISPCON_ISPEN		(1 << 0)
#define ISPCON_APUEN		(1 << 3)
#define ISPCON_ISPFF		(1 << 6)

#define ISPCMD_FCTRL		(0x2)
//#define ISPCMD_FCEN		(1 << 4)
#define ISPCMD_FOEN		(1 << 5)
// The above three terms combine to make the erase command
#define ISPCMD_ERASE		(ISPCMD_FCTRL |  ISPCMD_FOEN)

#define ISPTRG_ISPGO		(1 << 0)

/* access unlock keys */

#define KEY1			0x59
#define KEY2			0x16
#define KEY3			0x88
#define LOCK			0x00

// Private bank information for nucX1.
struct nucX1_flash_bank
{
	struct working_area *write_algorithm;	// this will be used later
	int probed;
};

// This is the function called in the config file.
FLASH_BANK_COMMAND_HANDLER(nucX1_flash_bank_command)
{
	struct nucX1_flash_bank *nucX1_info;

	if (CMD_ARGC < 6)
	{
		LOG_WARNING("incomplete flash_bank nucX1 configuration");
		return ERROR_FLASH_BANK_INVALID;
	}
	
	nucX1_info = malloc(sizeof(struct nucX1_flash_bank));
	bank->driver_priv = nucX1_info;

	nucX1_info->write_algorithm = NULL;
	nucX1_info->probed = 0;

	return ERROR_OK;
}

// Protection checking - examines the lock bit.
static int nucX1_protect_check(struct flash_bank *bank)
{
	struct target *target = bank->target;
	//struct stm32x_flash_bank *nucX1_info = bank->driver_priv;

	uint32_t protected;
	int s;
	int set;

	if (target->state != TARGET_HALTED)
	{
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	// Check to see if Nuc is unlocked or not
	int retval = target_read_u32(target, NUCX1_SYS_WRPROT, &protected);
	if (retval != ERROR_OK)
		return retval;
	LOG_INFO("protected = 0x%08" PRIx32 "", protected);
	if (protected == 0){	// means protected 
		set = 1;
	}
	else {
		set = 0;
	}
	for (s = 0; s < bank->num_sectors; s++)
				bank->sectors[s].is_protected = set;

	return ERROR_OK;
}

// The erase routine - active development is here.
// As of 7/31/11, this does not work. 
static int nucX1_erase(struct flash_bank *bank, int first, int last)
{
	struct target *target = bank->target;
	int i, timeout;
	//uint32_t protected;
	uint32_t protected, clockSelection, status, dummy;

	if (bank->target->state != TARGET_HALTED)
	{
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	LOG_INFO("NucX1: Sector Erase begins.");

	// Check to see if Nuc is unlocked or not
	int retval = target_read_u32(target, NUCX1_SYS_WRPROT, &protected);
	if (retval != ERROR_OK)
		return retval;
	LOG_INFO("protected = 0x%08" PRIx32 "", protected);
	if (protected == 0){	// means protected - so unlock it
		/* unlock flash registers */
		retval = target_write_u32(target,  NUCX1_SYS_WRPROT, KEY1);
		if (retval != ERROR_OK)
			return retval;
		retval = target_write_u32(target,  NUCX1_SYS_WRPROT, KEY2);
		if (retval != ERROR_OK)
			return retval;
		retval = target_write_u32(target,  NUCX1_SYS_WRPROT, KEY3);
		if (retval != ERROR_OK)
			return retval;
	}
	// Check that unlock worked
	retval = target_read_u32(target, NUCX1_SYS_WRPROT, &protected);
	if (retval != ERROR_OK)
		return retval;
	LOG_INFO("protected = 0x%08" PRIx32 "", protected);
	if (protected == 1){	// means unprotected
		LOG_INFO("protection removed\n");
	} else {
		LOG_INFO("still protected!!\n");
	}
	// select the internal clock and enable programming
	retval = target_read_u32(target, NUCX1_SYSCLK_PWRCON, &clockSelection);
	if (retval != ERROR_OK)
		return retval;
	LOG_INFO("clock selection = 0x%08" PRIx32 "", clockSelection);
	if ((clockSelection & PWRCON_XTL12M ) == 0){	// internal 22MHz not selected - select it
		retval = target_write_u32(target,  NUCX1_SYSCLK_PWRCON, PWRCON_XTL12M);
		if (retval != ERROR_OK)
			return retval;
		// Delay to allow settling
		alive_sleep(5);	// can use busy sleep for short times - but what's short??
		LOG_INFO("12MHz clock is now selected\n");
	}
	else {
		LOG_INFO("12MHz clock already selected\n");
	}
	retval = target_write_u32(target,  NUCX1_SYSCLK_CLKSEL0, 0x00);	// try all 0s - may be wrong?
	if (retval != ERROR_OK)
		return retval;
	retval = target_write_u32(target,  NUCX1_SYSCLK_CLKDIV, 0x00);	// all 0s are OK.
	if (retval != ERROR_OK)
		return retval;

	// most of these, I need to read the value, then set or clear the bit, then write it back out
	retval = target_write_u32(target,  NUCX1_SYSCLK_AHBCLK, AHBCLK_ISP_EN);	// should be the only bit set
	if (retval != ERROR_OK)
		return retval;
	retval = target_read_u32(target, NUCX1_FLASH_ISPCON, &dummy);
	if (retval != ERROR_OK)
		return retval;
	LOG_INFO("ISPCON = 0x%08" PRIx32 "", dummy);
	//dummy = dummy | ISPCON_ISPEN | ISPCON_APUEN;
	dummy = dummy | ISPCON_ISPEN ;
	LOG_INFO("ISPCON becomes 0x%08" PRIx32 "", dummy);
	retval = target_write_u32(target,  NUCX1_FLASH_ISPCON, dummy);
	if (retval != ERROR_OK)
		return retval;
/*
	retval = target_write_u32(target,  NUCX1_FLASH_ISPCON, ISPCON_ISPEN);
	if (retval != ERROR_OK)
		return retval;
	retval = target_write_u32(target,  NUCX1_FLASH_ISPCON, ISPCON_APUEN);	// should these OR in??
	if (retval != ERROR_OK)
		return retval;
*/
	LOG_INFO("ISPCMD gets 0x%08" PRIx32 "", ISPCMD_ERASE);
	retval = target_write_u32(target,  NUCX1_FLASH_ISPCMD, ISPCMD_ERASE);	// This is the whole command
	if (retval != ERROR_OK)
		return retval;

	for (i = first; i <= last; i++)
	{
		LOG_INFO("erasing sector %d at addresss 0x%" PRIx32 "\n",i,bank->base + bank->sectors[i].offset);
		
		retval = target_write_u32(target, NUCX1_FLASH_ISPADR, bank->base + bank->sectors[i].offset); // need size here??
		if (retval != ERROR_OK)
			return retval;
		retval = target_write_u32(target, NUCX1_FLASH_ISPTRG, ISPTRG_ISPGO); // This is the only bit available
		if (retval != ERROR_OK)
			return retval;

		//wait for busy to clear - check the GO flag 
		timeout = 100;
		for (;;)
		{
			retval = target_read_u32(target, NUCX1_FLASH_ISPTRG, &status);
			if (retval != ERROR_OK)
				return retval;
			LOG_INFO("status: 0x%" PRIx32 "", status);
			if (status == 0)
				break;
			if (timeout-- <= 0)
			{
				LOG_INFO("timed out waiting for flash");
				return ERROR_FAIL;
			}
			busy_sleep(1);	// can use busy sleep for short times.
		}
		// check for failure
		retval = target_read_u32(target, NUCX1_FLASH_ISPCON, &status);
		if (retval != ERROR_OK)
			return retval;
		if ((status & ISPCON_ISPFF) != 0){
			LOG_DEBUG("failure: 0x%" PRIx32 "", status);
			// if bit is set, then must write to it to clear it.
			retval = target_write_u32(target,  NUCX1_FLASH_ISPCON, ISPCON_ISPFF);
			if (retval != ERROR_OK)
				return retval;
		} else {
			LOG_INFO ("erased OK\n");
			bank->sectors[i].is_erased = 1;
		}
	}
	// done, so restore the protection
	retval = target_write_u32(target,  NUCX1_SYS_WRPROT, LOCK);
	if (retval != ERROR_OK)
		return retval;
	LOG_INFO("Erase done\n" );

	return ERROR_OK;
}

// The write routine stub.
static int nucX1_write_block(struct flash_bank *bank, uint8_t *buffer, uint32_t offset, uint32_t count)
{

	int retval = ERROR_OK;

	LOG_INFO("Novoton NUC: FLASH Write ...");


	return retval;
}

// The probe routine for the nuc. Only recognizes the nuc120 right now.
static int nucX1_probe(struct flash_bank *bank)
{
	struct target *target = bank->target;
	struct nucX1_flash_bank *nucX1_info = bank->driver_priv;
	int i;
	uint16_t num_pages;
	uint32_t device_id;
	int page_size;
	uint32_t base_address = 0x00000000;

	nucX1_info->probed = 0;

	// don't know for sure if this is required??
	if (bank->target->state != TARGET_HALTED)
	{
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	/* read nucX1 device id register */
	int retval = target_read_u32(target, 0x50000000, &device_id);
	if (retval != ERROR_OK)
		return retval;
	LOG_INFO("device id = 0x%08" PRIx32 "", device_id);

	if ((device_id ) == 0x00012000)
	{
		/* medium density - we have 1k pages
		 * 4 pages for a protection area */
		page_size = 512;		// This may be better thought of as "sectors"
		num_pages = 256;	// 128K
		LOG_INFO("Nuc 120 Type");
		//nucX1_info->ppage_size = 4;
	}
	else if ((device_id ) != 0x00000000)	// probably a nuc of some sort
	{
		
		page_size = 512;		// this is the standard size
		//nucX1_info->ppage_size = 4;
		num_pages = 64;		// 32K (Arbitrary)

		LOG_WARNING("Undefined NUC type??");
	}
	
	else
	{
		LOG_WARNING("Cannot identify target as a nuc family.");
		return ERROR_FAIL;
	}

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
		//LOG_INFO("sector id = %x", i);
	}

	nucX1_info->probed = 1;
	
  	LOG_INFO("Novoton NUC: Probed ...");


	return ERROR_OK;
}

// Standard approach to autoprobing.
static int nucX1_auto_probe(struct flash_bank *bank)
{
	struct nucX1_flash_bank *nucX1_info = bank->driver_priv;
	if (nucX1_info->probed)
		return ERROR_OK;
	return nucX1_probe(bank);
}

// Info doesn't really add much, but works correctly.
static int nucX1_info(struct flash_bank *bank, char *buf, int buf_size)
{
	struct target *target = bank->target;
	uint32_t device_id;
	int printed;

	/* read nucX1 device id register */
	int retval = target_read_u32(target, 0x50000000, &device_id);
	if (retval != ERROR_OK)
		return retval;

	if ((device_id) == 0x00012000)
	{
		LOG_INFO("nuc120USB (Medium Density)");
	}
	else if ((device_id) != 0x00000000)
	{
		LOG_INFO("nuc device likely - add to driver");
	}
	
	else
	{
		LOG_INFO("Cannot identify target as a nuc1xx");
		return ERROR_FAIL;
	}

	return ERROR_OK;
}
/*
// The nuc120 doesn't support mass erase, so this will probably be removed soon.
// The structure is left for now until I am sure I don't want to add any custom
//  commands.
static int nucX1_mass_erase(struct flash_bank *bank)
{
	struct target *target = bank->target;
	int retval = ERROR_OK;
	
	//uint32_t u32DummyRead;
	//int32_t flashflag;
	//uint32_t ucDQ6_read1, ucDQ6_read2, ucDQ5_read;

	if (target->state != TARGET_HALTED)
	{
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	LOG_INFO("Novoton NUC: Chip Erase ... (may take several seconds)");

       
	return retval;
}

COMMAND_HANDLER(nucX1_handle_mass_erase_command)
{
	int i;	// for erasing sectors
	if (CMD_ARGC < 1)
	{
		command_print(CMD_CTX, "nucX1 mass_erase <bank>");
		return ERROR_OK;
	}

	struct flash_bank *bank;
	int retval = CALL_COMMAND_HANDLER(flash_command_get_bank, 0, &bank);
	if (ERROR_OK != retval)
		return retval;

	retval = nucX1_mass_erase(bank);
	if (retval == ERROR_OK)
	{
		// set all sectors as erased 
		for (i = 0; i < bank->num_sectors; i++)
		{
			bank->sectors[i].is_erased = 1;
		}

		command_print(CMD_CTX, "nucX1 mass erase complete");
	}
	else
	{
		command_print(CMD_CTX, "nucX1 mass erase failed");
	}

	return retval;
}


static const struct command_registration nucX1_exec_command_handlers[] = {
	{
		.name = "mass_erase",
		.handler = nucX1_handle_mass_erase_command,
		.mode = COMMAND_EXEC,
		.usage = "bank_id",
		.help = "Erase entire Flash device.",
	},
	COMMAND_REGISTRATION_DONE
};

static const struct command_registration nucX1_command_handlers[] = {
	{
		.name = "nucX1",
		.mode = COMMAND_ANY,
		.help = "nucX1 Flash command group",
		.chain = nucX1_exec_command_handlers,
	},
	COMMAND_REGISTRATION_DONE
};
*/

struct flash_driver nucX1_flash = {
	.name = "nucX1",
	//.commands = nucX1_command_handlers,
	.commands = NULL,
	.flash_bank_command = nucX1_flash_bank_command,
	.erase = nucX1_erase,
	.write = nucX1_write_block,
	.read = default_flash_read,
	.probe = nucX1_probe,
	.auto_probe = nucX1_auto_probe,
	.erase_check = default_flash_mem_blank_check,
	.protect_check = nucX1_protect_check,
	.info = nucX1_info,
};
