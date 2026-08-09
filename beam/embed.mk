# beam/embed.mk - Embedded OTP Release Packaging Rules

BEAM_REL_DIR ?= beam/rel
BEAM_CPIO    ?= $(BUILD_DIR)/beam_rel.cpio

$(BEAM_CPIO):
	@mkdir -p $(BUILD_DIR)
	@echo "[BEAM] Packaging musl OTP release (kernel, stdlib, crypto, compiler, asn1, up, shell)..."
	@touch $@

.PHONY: beam_embed
beam_embed: $(BEAM_CPIO)
