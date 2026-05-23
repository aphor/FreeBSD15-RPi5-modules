/*
 * cyw43455_fwmod.c — firmware registrar KLD for CYW43455 SDIO WiFi
 *
 * Registers brcmfmac43455-sdio.bin, brcmfmac43455-sdio.txt, and
 * brcmfmac43455-sdio.clm_blob with the kernel firmware(9) subsystem so
 * that cyw43455.ko can retrieve them via firmware_get().
 *
 * The firmware blobs are linked in via ld(1) -b binary; the symbols are:
 *   _binary_brcmfmac43455_sdio_bin_{start,end,size}
 *   _binary_brcmfmac43455_sdio_txt_{start,end,size}
 *   _binary_brcmfmac43455_sdio_clm_blob_{start,end,size}
 *
 * Build: see Makefile.cyw43455fw (requires firmware files in the build dir)
 * Install: /boot/modules/cyw43455fw.ko (loaded before cyw43455.ko)
 */

#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/firmware.h>

/* Symbols injected by ld -b binary */
extern const uint8_t _binary_brcmfmac43455_sdio_bin_start[];
extern const uint8_t _binary_brcmfmac43455_sdio_bin_end[];
extern const uint8_t _binary_brcmfmac43455_sdio_txt_start[];
extern const uint8_t _binary_brcmfmac43455_sdio_txt_end[];
extern const uint8_t _binary_brcmfmac43455_sdio_clm_blob_start[];
extern const uint8_t _binary_brcmfmac43455_sdio_clm_blob_end[];

static const struct firmware *cyw_fw_bin;
static const struct firmware *cyw_fw_txt;
static const struct firmware *cyw_fw_clm;

static int
cyw43455fw_modevent(module_t mod, int type, void *unused)
{
	switch (type) {
	case MOD_LOAD:
		cyw_fw_bin = firmware_register("brcmfmac43455-sdio.bin",
		    _binary_brcmfmac43455_sdio_bin_start,
		    _binary_brcmfmac43455_sdio_bin_end -
		    _binary_brcmfmac43455_sdio_bin_start,
		    1, NULL);
		if (cyw_fw_bin == NULL)
			return (ENOMEM);

		cyw_fw_txt = firmware_register("brcmfmac43455-sdio.txt",
		    _binary_brcmfmac43455_sdio_txt_start,
		    _binary_brcmfmac43455_sdio_txt_end -
		    _binary_brcmfmac43455_sdio_txt_start,
		    1, NULL);
		if (cyw_fw_txt == NULL) {
			firmware_unregister("brcmfmac43455-sdio.bin");
			return (ENOMEM);
		}

		cyw_fw_clm = firmware_register("brcmfmac43455-sdio.clm_blob",
		    _binary_brcmfmac43455_sdio_clm_blob_start,
		    _binary_brcmfmac43455_sdio_clm_blob_end -
		    _binary_brcmfmac43455_sdio_clm_blob_start,
		    1, NULL);
		if (cyw_fw_clm == NULL) {
			firmware_unregister("brcmfmac43455-sdio.txt");
			firmware_unregister("brcmfmac43455-sdio.bin");
			return (ENOMEM);
		}
		return (0);

	case MOD_UNLOAD:
		firmware_unregister("brcmfmac43455-sdio.clm_blob");
		firmware_unregister("brcmfmac43455-sdio.txt");
		firmware_unregister("brcmfmac43455-sdio.bin");
		return (0);
	}
	return (EINVAL);
}

static moduledata_t cyw43455fw_mod = {
	"cyw43455fw",
	cyw43455fw_modevent,
	0
};
DECLARE_MODULE(cyw43455fw, cyw43455fw_mod, SI_SUB_DRIVERS, SI_ORDER_ANY);
MODULE_VERSION(cyw43455fw, 1);
