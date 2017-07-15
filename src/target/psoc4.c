#include "general.h"
#include "target.h"
#include "target_internal.h"
#include "cortexm.h"
#include "exception.h"

#define PSOC4_SROM_KEY1			    (0xb6UL)
#define PSOC4_SROM_KEY2			    (0xd3UL)
#define PSOC4_SROM_SYSREQ_BIT	    (1UL<<31)
#define PSOC4_SROM_HMASTER_BIT		(1UL<<30)
#define PSOC4_SROM_PRIVILEGED_BIT	(1UL<<28)
#define PSOC4_SROM_STATUS_SUCCEEDED	(0xa0000000)
#define PSOC4_SROM_STATUS_FAILED	(0xf0000000)

#define PSOC4_CMD_GET_SILICON_ID	      (0x00)
#define PSOC4_SROM_CMD_LOAD_LATCH      (0x04)
#define PSOC4_SROM_CMD_PROGRAM_ROW     (0x06)
#define PSOC4_SROM_CMD_CHECKSUM        (0x0B)
#define PSOC4_SROM_CMD_WRITE_PROTECTION	   (0x0d)
#define PSOC4_CMD_ERASE_ALL			   (0x0a)
#define PSOC4_SROM_CMD_SET_IMO_48MHZ   (0x15)

#define PSOC4_CPUSS_SYSARG  	(0x40100008)
#define PSOC4_CPUSS_SYSREQ  	(0x40100004)
#define PSOC4_SRAM_PARAMS_BASE  (0x20000100)

#define PSOC4_CHIP_PROT_VIRGIN		(0x0)
#define PSOC4_CHIP_PROT_OPEN		   (0x1)
#define PSOC4_CHIP_PROT_PROTECTED	(0x2)
#define PSOC4_CHIP_PROT_KILL		   (0x4)

#define PSOC4_ROW_SIZE           (128UL)
#define PSOC4_ROWS_PER_MACRO     (512UL)
#define PSOC4_MACRO_SIZE         (PSOC4_ROW_SIZE * PSOC4_ROWS_PER_MACRO)

#define PSOC4_IDCODE              (0x0BB11477)
#define PSOC4_TEST_MODE           (0x40030014)

#define read_io(_t, _a) target_mem_read32(_t, _a)
#define write_io(_t, _a, _v) target_mem_write32(_t, _a, _v)

static bool psoc4_cmd_erase_mass(target *t);
static bool psoc4_cmd_checksum(target *t);
static bool psoc4_cmd_siliconid(target *t);

const struct command_s psoc4_cmd_list[] = {
		{"erase_mass", (cmd_handler)psoc4_cmd_erase_mass, "Erase entire flash memory"},
		{"checksum", (cmd_handler)psoc4_cmd_checksum, "Print Flash checksum"},
		{"siliconid", (cmd_handler)psoc4_cmd_siliconid, "Print silicon id"},
		{NULL, NULL, NULL}
};


static int psoc4_flash_erase(struct target_flash *f,
		target_addr addr, size_t len);
static int psoc4_flash_write(struct target_flash *f,
		target_addr dest,
		const void* src,
		size_t size);
static int psoc4_prot_write(struct target_flash *f,
		target_addr dest,
		const void* src,
		size_t size);

static void psoc4_add_flash(target *t,
		uint32_t addr, size_t length, size_t erasesize)
{
	struct target_flash *f = calloc(1, sizeof(*f));
	f->start = addr;
	f->length = length;
	f->blocksize = erasesize;
	f->erase = psoc4_flash_erase;
	if(addr == 0x90400000)
		f->write = psoc4_prot_write;
	else
		f->write = psoc4_flash_write;
	f->buf_size = erasesize;
	target_add_flash(t, f);
}

static bool poll_srom_status(target* t)
{
	uint32_t status;
	platform_timeout tmout;
	platform_timeout_set(&tmout, 1000);

	do{
		status = read_io(t, PSOC4_CPUSS_SYSREQ);
		status &= (PSOC4_SROM_SYSREQ_BIT | PSOC4_SROM_PRIVILEGED_BIT);
	}while ((status != 0) && (!platform_timeout_is_expired(&tmout)));

	if (platform_timeout_is_expired(&tmout)) {
		tc_printf(t, "SROM poll timeout!!!\n");
		return false;
	}

	status = read_io(t, PSOC4_CPUSS_SYSARG);

	if ((status & 0xF0000000) != (PSOC4_SROM_STATUS_SUCCEEDED))
	{
		tc_printf(t, "SROM poll status error: 0x%08x!!!\n");
		return false;
	}
	else
		return true;
}

static int psoc4_flash_erase(struct target_flash *f,
		target_addr addr, size_t len)
{
	(void)f;
	(void)addr;
	(void)len;
	return 0;
}

static bool psoc4_load_latch(target *t,  uint8_t macro_id, const void* src, size_t size)
{
	uint8_t i;

	uint32_t params1 = (PSOC4_SROM_KEY1 << 0) +
			((PSOC4_SROM_KEY2 + PSOC4_SROM_CMD_LOAD_LATCH) << 8) +
			(0x00 << 16)+
			(macro_id << 24);

	uint32_t params2 = (size - 1);
	uint32_t *src_data = (uint32_t *)src;

	write_io(t, PSOC4_SRAM_PARAMS_BASE + 0x00, params1);
	write_io(t, PSOC4_SRAM_PARAMS_BASE + 0x04, params2);

	for (i = 0; i < size; i += 4) {
		params1 = *src_data;
		src_data++;

		write_io(t, PSOC4_SRAM_PARAMS_BASE + 0x08 + i, params1);
	}

	write_io(t, PSOC4_CPUSS_SYSARG, PSOC4_SRAM_PARAMS_BASE);
	write_io(t, PSOC4_CPUSS_SYSREQ, PSOC4_SROM_SYSREQ_BIT | PSOC4_SROM_CMD_LOAD_LATCH);

	return poll_srom_status(t);

}

static int psoc4_flash_write(struct target_flash *f,
		target_addr dest,
		const void* src,
		size_t size)
{
	int resp = 0;
	target *t = f->t;
	uint32_t row_id = dest / PSOC4_ROW_SIZE;
	uint32_t macro_id = row_id / PSOC4_ROWS_PER_MACRO;

	if(dest % f->blocksize)
		return -1;

	if(size != f->blocksize)
		return -1;

	target_halt_resume(t, false);

	if(!psoc4_load_latch(t, macro_id, src, size))
	{
		resp = -1;
	}
	else
	{
		uint32_t params = (PSOC4_SROM_KEY1 << 0) +
				((PSOC4_SROM_KEY2 + PSOC4_SROM_CMD_PROGRAM_ROW) << 8) +
				((row_id & 0x00FF) << 16) +
				((row_id & 0xFF00) << 16);

		write_io(t, PSOC4_SRAM_PARAMS_BASE+0x00, params);
		write_io(t, PSOC4_CPUSS_SYSARG, PSOC4_SRAM_PARAMS_BASE);
		write_io(t, PSOC4_CPUSS_SYSREQ, PSOC4_SROM_SYSREQ_BIT | PSOC4_SROM_CMD_PROGRAM_ROW);

		if(!poll_srom_status(t))
			resp = -1;
	}

	target_halt_request(t);
	return resp;
}

static int psoc4_prot_write(struct target_flash *f,
		target_addr dest,
		const void* src,
		size_t size)
{
	int resp = 0;
	target *t = f->t;

	if(dest % f->blocksize)
		return -1;

	if(size != f->blocksize)
		return -1;

	uint32_t macro_id = (dest - f->start) / f->blocksize;
	target_halt_resume(t, false);

	if(!psoc4_load_latch(t, macro_id, src, size))
	{
		resp = -1;
	}
	else
	{
		uint32_t params = (PSOC4_SROM_KEY1 << 0) +
				((PSOC4_SROM_KEY2 + PSOC4_SROM_CMD_WRITE_PROTECTION) << 8) +
				(0x01 << 16) +
				(macro_id << 24);

		write_io(t, PSOC4_CPUSS_SYSARG, params);
		write_io(t, PSOC4_CPUSS_SYSREQ, PSOC4_SROM_SYSREQ_BIT | PSOC4_SROM_CMD_WRITE_PROTECTION);

		if(!poll_srom_status(t))
			resp = -1;
	}

	target_halt_request(t);
	return resp;
}

static bool psoc4_cmd_erase_mass(target *t)
{
	bool resp = true;
	uint32_t params = PSOC4_SROM_KEY1
			| ((PSOC4_SROM_KEY2 + PSOC4_CMD_GET_SILICON_ID) << 8);

	target_halt_resume(t, false);

	write_io(t, PSOC4_CPUSS_SYSARG,  params);
	write_io(t, PSOC4_CPUSS_SYSREQ,  PSOC4_SROM_SYSREQ_BIT | PSOC4_CMD_GET_SILICON_ID);

	if(!poll_srom_status(t))
	{
		resp = false;
	}
	else
	{

		uint8_t chip_prot = read_io(t, PSOC4_CPUSS_SYSREQ) >> 12;
		if(chip_prot == PSOC4_CHIP_PROT_PROTECTED)
		{
			tc_printf(t, "Chip is protected\n");
			params = (PSOC4_SROM_KEY1 << 0) +
					((PSOC4_SROM_KEY2 + PSOC4_SROM_CMD_WRITE_PROTECTION) << 8) +
					(0x01 << 16) +
					(0x00 << 24);

			write_io(t, PSOC4_CPUSS_SYSARG, params);
			write_io(t, PSOC4_CPUSS_SYSREQ, PSOC4_SROM_SYSREQ_BIT | PSOC4_SROM_CMD_WRITE_PROTECTION);

			if(!poll_srom_status(t))
				resp = false;

			resp = false;
		} else {
			params = (PSOC4_SROM_KEY1 << 0) +
					((PSOC4_SROM_KEY2 + PSOC4_CMD_ERASE_ALL) << 8);

			write_io(t, PSOC4_SRAM_PARAMS_BASE + 0x00, params);
			write_io(t, PSOC4_CPUSS_SYSARG, PSOC4_SRAM_PARAMS_BASE);
			write_io(t, PSOC4_CPUSS_SYSREQ, PSOC4_SROM_SYSREQ_BIT | PSOC4_CMD_ERASE_ALL);

			if(!poll_srom_status(t))
				resp = false;
		}
	}

	target_halt_request(t);
	return resp;
}

static bool psoc4_cmd_checksum(target *t)
{
	bool resp = true;

	target_halt_resume(t, false);

	uint32_t params = (PSOC4_SROM_KEY1 << 0) +
			((PSOC4_SROM_KEY2 + PSOC4_SROM_CMD_CHECKSUM) << 8)+
			((0x0000 & 0x00FF) << 16) +
			((0x8000 & 0xFF00) << 16);

	target_halt_resume(t, false);
	write_io(t, PSOC4_CPUSS_SYSARG, params);
	write_io(t, PSOC4_CPUSS_SYSREQ,  PSOC4_SROM_SYSREQ_BIT |  PSOC4_SROM_CMD_CHECKSUM);

	if(!poll_srom_status(t))
		resp = false;
	else
	{
		uint32_t checksum = read_io(t, PSOC4_CPUSS_SYSARG) & 0x0FFFFFFF;
		tc_printf(t, "0x%08x\n", checksum);
	}

	target_halt_request(t);
	return resp;
}

static bool psoc4_cmd_siliconid(target *t)
{
	bool resp = true;
	uint32_t params = PSOC4_SROM_KEY1
			| ((PSOC4_SROM_KEY2 + PSOC4_CMD_GET_SILICON_ID) << 8);

	target_halt_resume(t, false);

	write_io(t, PSOC4_CPUSS_SYSARG,  params);
	write_io(t, PSOC4_CPUSS_SYSREQ,  PSOC4_SROM_SYSREQ_BIT | PSOC4_CMD_GET_SILICON_ID);

	if(!poll_srom_status(t))
	{
		resp = false;
	}
	else
	{
		uint32_t part0 = read_io(t, PSOC4_CPUSS_SYSARG);
		uint32_t part1 = read_io(t, PSOC4_CPUSS_SYSREQ);

		uint32_t siliconid = ((part0 >> 8) & 0xFF) +
				(((part0 >> 0) & 0xFF) << 8) +
				(((part0 >> 16) & 0xFF) << 16) +
				(((part1 >> 0) & 0xFF) << 24);

		tc_printf(t, "0x%08x\n", siliconid);
	}

	target_halt_request(t);
	return resp;
}

static bool psoc4_reset(target *t)
{
	// Empty for now
	// Probably something like:
	//   write_io(t, 0xE000ED0C, 0x05FA0004);
	(void)t;
	return true;
}

void psoc4_chip_acquire(ADIv5_DP_t *dp)
{
	if(dp->idcode == PSOC4_IDCODE)
	{
		adiv5_dp_write(dp, ADIV5_DP_CTRLSTAT, 0x54000000);
		adiv5_dp_write(dp, ADIV5_DP_SELECT, 0x00000000);
		adiv5_dp_write(dp, ADIV5_AP_CSW, 0x00000002);

		adiv5_dp_write(dp, ADIV5_AP_TAR, PSOC4_TEST_MODE);
		adiv5_dp_write(dp, ADIV5_AP_DRW, 0x80000000);

		adiv5_dp_write(dp, ADIV5_AP_TAR, PSOC4_TEST_MODE);
		adiv5_dp_read(dp, ADIV5_AP_DRW);
		uint32_t data = adiv5_dp_read(dp, ADIV5_AP_DRW);
		if((data & 0x80000000) == 0x80000000)
		{
			platform_timeout tmout;
			platform_timeout_set(&tmout, 1000);
			do
			{
				adiv5_dp_write(dp, ADIV5_AP_TAR, PSOC4_CPUSS_SYSREQ);
				adiv5_dp_read(dp, ADIV5_AP_DRW);
				data = adiv5_dp_read(dp, ADIV5_AP_DRW);
			} while(((data & PSOC4_SROM_PRIVILEGED_BIT) != 0x00000000) &&
					(!platform_timeout_is_expired(&tmout)));
		}
	}
}

bool psoc4_probe(target* t)
{
	uint32_t params = (PSOC4_SROM_KEY1 << 0) |
			((PSOC4_SROM_KEY2 + PSOC4_SROM_CMD_SET_IMO_48MHZ) << 8);

	write_io(t, PSOC4_CPUSS_SYSARG,  params);
	write_io(t, PSOC4_CPUSS_SYSREQ,  PSOC4_SROM_SYSREQ_BIT | PSOC4_SROM_CMD_SET_IMO_48MHZ);

	if(!poll_srom_status(t))
		return false;

	params = PSOC4_SROM_KEY1
			| ((PSOC4_SROM_KEY2 + PSOC4_CMD_GET_SILICON_ID) << 8);

	write_io(t, PSOC4_CPUSS_SYSARG,  params);
	write_io(t, PSOC4_CPUSS_SYSREQ,  PSOC4_SROM_SYSREQ_BIT | PSOC4_CMD_GET_SILICON_ID);

	if(!poll_srom_status(t))
		return false;

	t->idcode = read_io(t, PSOC4_CPUSS_SYSARG) & 0xFFFF;

	switch(t->idcode) {
	case 0xE51:
		t->driver = "CYBLE-012011-00";
		t->reset = (void*)psoc4_reset;
		target_add_ram(t, 0x20000000, 0x4000);
		psoc4_add_flash(t, 0x00000000, 0x20000, 0x80);
		psoc4_add_flash(t, 0x90400000, 128, 64);
		target_add_commands(t, psoc4_cmd_list, "PSoC4");
		return true;
	}

	return false;
}
