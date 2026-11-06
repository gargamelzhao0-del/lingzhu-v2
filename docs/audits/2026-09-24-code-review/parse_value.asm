
C:/tmp/nuwax-playback-startup-v1-20260923/NuwaxAIPartner.elf:     file format elf32-xtensa-le


Disassembly of section .rtc.text:

Disassembly of section .iram0.vectors:

Disassembly of section .iram0.text:

Disassembly of section .flash.text:

4213eb64 <parse_value>:
4213eb64:	00a136        	entry	a1, 80
4213eb67:	1e5316        	beqz	a3, 4213ed50 <parse_value+0x1ec>
4213eb6a:	03d8      	l32i.n	a13, a3, 0
4213eb6c:	1e0d16        	beqz	a13, 4213ed50 <parse_value+0x1ec>
4213eb6f:	2388      	l32i.n	a8, a3, 8
4213eb71:	1398      	l32i.n	a9, a3, 4
4213eb73:	e84b      	addi.n	a14, a8, 4
4213eb75:	2939e7        	bltu	a9, a14, 4213eba2 <parse_value+0x3e>
4213eb78:	6199      	s32i.n	a9, a1, 24
4213eb7a:	b941b1        	l32r	a11, 4212d080 <dl_nn_get_tanh_table+0x43c> (3c24f884 <_flash_rodata_start+0x2f764>)
4213eb7d:	9d8a      	add.n	a9, a13, a8
4213eb7f:	09ad      	mov.n	a10, a9
4213eb81:	4c0c      	movi.n	a12, 4
4213eb83:	5189      	s32i.n	a8, a1, 20
4213eb85:	41d9      	s32i.n	a13, a1, 16
4213eb87:	097d      	mov.n	a7, a9
4213eb89:	71e9      	s32i.n	a14, a1, 28
4213eb8b:	b95181        	l32r	a8, 4212d0d0 <dl_nn_get_tanh_table+0x48c> (4000123c <strncmp>)
4213eb8e:	0008e0        	callx8	a8
4213eb91:	41d8      	l32i.n	a13, a1, 16
4213eb93:	5188      	l32i.n	a8, a1, 20
4213eb95:	6198      	l32i.n	a9, a1, 24
4213eb97:	71e8      	l32i.n	a14, a1, 28
4213eb99:	0adc      	bnez.n	a10, 4213ebad <parse_value+0x49>
4213eb9b:	480c      	movi.n	a8, 4
4213eb9d:	3289      	s32i.n	a8, a2, 12
4213eb9f:	00d206        	j	4213eeeb <parse_value+0x387>
4213eba2:	e85b      	addi.n	a14, a8, 5
4213eba4:	0239e7        	bltu	a9, a14, 4213ebaa <parse_value+0x46>
4213eba7:	00db46        	j	4213ef18 <parse_value+0x3b4>
4213ebaa:	000f86        	j	4213ebec <parse_value+0x88>
4213ebad:	f85b      	addi.n	a15, a8, 5
4213ebaf:	0239f7        	bltu	a9, a15, 4213ebb5 <parse_value+0x51>
4213ebb2:	00e1c6        	j	4213ef3d <parse_value+0x3d9>
4213ebb5:	000206        	j	4213ebc1 <parse_value+0x5d>
4213ebb8:	0fed      	mov.n	a14, a15
4213ebba:	180c      	movi.n	a8, 1
4213ebbc:	3289      	s32i.n	a8, a2, 12
4213ebbe:	00ca46        	j	4213eeeb <parse_value+0x387>
4213ebc1:	b931b1        	l32r	a11, 4212d088 <dl_nn_get_tanh_table+0x444> (3c2687c8 <_flash_rodata_start+0x486a8>)
4213ebc4:	4c0c      	movi.n	a12, 4
4213ebc6:	07ad      	mov.n	a10, a7
4213ebc8:	71e9      	s32i.n	a14, a1, 28
4213ebca:	6199      	s32i.n	a9, a1, 24
4213ebcc:	5189      	s32i.n	a8, a1, 20
4213ebce:	41d9      	s32i.n	a13, a1, 16
4213ebd0:	b94081        	l32r	a8, 4212d0d0 <dl_nn_get_tanh_table+0x48c> (4000123c <strncmp>)
4213ebd3:	0008e0        	callx8	a8
4213ebd6:	41d8      	l32i.n	a13, a1, 16
4213ebd8:	5188      	l32i.n	a8, a1, 20
4213ebda:	6198      	l32i.n	a9, a1, 24
4213ebdc:	71e8      	l32i.n	a14, a1, 28
4213ebde:	aacc      	bnez.n	a10, 4213ebec <parse_value+0x88>
4213ebe0:	280c      	movi.n	a8, 2
4213ebe2:	3289      	s32i.n	a8, a2, 12
4213ebe4:	180c      	movi.n	a8, 1
4213ebe6:	5289      	s32i.n	a8, a2, 20
4213ebe8:	00bfc6        	j	4213eeeb <parse_value+0x387>
4213ebeb:	00          	.byte	00
4213ebec:	023897        	bltu	a8, a9, 4213ebf2 <parse_value+0x8e>
4213ebef:	005746        	j	4213ed50 <parse_value+0x1ec>
4213ebf2:	80fd80        	add	a15, a13, a8
4213ebf5:	000fb2        	l8ui	a11, a15, 0
4213ebf8:	22a0a2        	movi	a10, 34
4213ebfb:	0d9ba7        	bne	a11, a10, 4213ec0c <parse_value+0xa8>
4213ebfe:	20a220        	or	a10, a2, a2
4213ec01:	03bd      	mov.n	a11, a3
4213ec03:	ff43a5        	call8	4213e03c <parse_string>
4213ec06:	0a2d      	mov.n	a2, a10
4213ec08:	00d606        	j	4213ef64 <parse_value+0x400>
4213ec0b:	cbc200        	movf.s	f12, f2, b0
4213ec0e:	c0c0d0        	sub	a12, a0, a13
4213ec11:	9a0c74        	ee.vst.128.ip	q2, a7, 192
4213ec14:	62aac0        	saltu	a10, a10, a12
4213ec17:	aa0b      	addi.n	a10, a10, -1
4213ec19:	60a0a0        	neg	a10, a10
4213ec1c:	74a0a0        	extui	a10, a10, 0, 8
4213ec1f:	9acc      	bnez.n	a10, 4213ec2c <parse_value+0xc8>
4213ec21:	d3cbe2        	addi	a14, a11, -45
4213ec24:	1c0c      	movi.n	a12, 1
4213ec26:	93cae0        	movnez	a12, a10, a14
4213ec29:	0d9c16        	beqz	a12, 4213ed06 <parse_value+0x1a2>
4213ec2c:	0c0c      	movi.n	a12, 0
4213ec2e:	b924e1        	l32r	a14, 4212d0c0 <dl_nn_get_tanh_table+0x47c> (4007fed <_strtok_last+0x3e9f74d>)
4213ec31:	01c9      	s32i.n	a12, a1, 0
4213ec33:	5b4c      	movi.n	a11, 69
4213ec35:	ad2c      	movi.n	a13, 42
4213ec37:	c09980        	sub	a9, a9, a8
4213ec3a:	1b8976        	loop	a9, 4213ec59 <parse_value+0xf5>
4213ec3d:	8fca      	add.n	a8, a15, a12
4213ec3f:	000882        	l8ui	a8, a8, 0
4213ec42:	d5c8a2        	addi	a10, a8, -43
4213ec45:	083b87        	bltu	a11, a8, 4213ec51 <parse_value+0xed>
4213ec48:	0dbd87        	bgeu	a13, a8, 4213ec59 <parse_value+0xf5>
4213ec4b:	08dea7        	bbs	a14, a10, 4213ec57 <parse_value+0xf3>
4213ec4e:	0001c6        	j	4213ec59 <parse_value+0xf5>
4213ec51:	65a0a2        	movi	a10, 101
4213ec54:	0198a7        	bne	a8, a10, 4213ec59 <parse_value+0xf5>
4213ec57:	cc1b      	addi.n	a12, a12, 1
4213ec59:	4388      	l32i.n	a8, a3, 16
4213ec5b:	ac1b      	addi.n	a10, a12, 1
4213ec5d:	41c9      	s32i.n	a12, a1, 16
4213ec5f:	0008e0        	callx8	a8
4213ec62:	41c8      	l32i.n	a12, a1, 16
4213ec64:	1acc      	bnez.n	a10, 4213ec69 <parse_value+0x105>
4213ec66:	003986        	j	4213ed50 <parse_value+0x1ec>
4213ec69:	2398      	l32i.n	a9, a3, 8
4213ec6b:	03b8      	l32i.n	a11, a3, 0
4213ec6d:	51c9      	s32i.n	a12, a1, 20
4213ec6f:	bb9a      	add.n	a11, a11, a9
4213ec71:	41a9      	s32i.n	a10, a1, 16
4213ec73:	b85181        	l32r	a8, 4212cdb8 <dl_nn_get_tanh_table+0x174> (400011f4 <memcpy>)
4213ec76:	0008e0        	callx8	a8
4213ec79:	4188      	l32i.n	a8, a1, 16
4213ec7b:	51c8      	l32i.n	a12, a1, 20
4213ec7d:	090c      	movi.n	a9, 0
4213ec7f:	c8ca      	add.n	a12, a8, a12
4213ec81:	004c92        	s8i	a9, a12, 0
4213ec84:	08ad      	mov.n	a10, a8
4213ec86:	01bd      	mov.n	a11, a1
4213ec88:	b91381        	l32r	a8, 4212d0d4 <dl_nn_get_tanh_table+0x490> (421f557c <strtod>)
4213ec8b:	0008e0        	callx8	a8
4213ec8e:	0198      	l32i.n	a9, a1, 0
4213ec90:	4188      	l32i.n	a8, a1, 16
4213ec92:	5358      	l32i.n	a5, a3, 20
4213ec94:	0a6d      	mov.n	a6, a10
4213ec96:	0b7d      	mov.n	a7, a11
4213ec98:	089897        	bne	a8, a9, 4213eca4 <parse_value+0x140>
4213ec9b:	08ad      	mov.n	a10, a8
4213ec9d:	0005e0        	callx8	a5
4213eca0:	002b06        	j	4213ed50 <parse_value+0x1ec>
4213eca3:	09c100        	l32e	a0, a1, -16
4213eca6:	d1b9      	s32i.n	a11, a1, 52
4213eca8:	b909      	s32i.n	a0, a9, 44
4213ecaa:	5199      	s32i.n	a9, a1, 20
4213ecac:	4189      	s32i.n	a8, a1, 16
4213ecae:	b90a81        	l32r	a8, 4212d0d8 <dl_nn_get_tanh_table+0x494> (40002388 <__gedf2>)
4213ecb1:	0008e0        	callx8	a8
4213ecb4:	b877c1        	l32r	a12, 4212ce90 <dl_nn_get_tanh_table+0x24c> (7fffffff <_rtc_reserved_end+0x1fefffff>)
4213ecb7:	4188      	l32i.n	a8, a1, 16
4213ecb9:	5198      	l32i.n	a9, a1, 20
4213ecbb:	029ad6        	bgez	a10, 4213ece8 <parse_value+0x184>
4213ecbe:	b901d1        	l32r	a13, 4212d0c4 <dl_nn_get_tanh_table+0x480> (c1e00000 <_rtc_reserved_end+0x61d00000>)
4213ecc1:	0c0c      	movi.n	a12, 0
4213ecc3:	06ad      	mov.n	a10, a6
4213ecc5:	07bd      	mov.n	a11, a7
4213ecc7:	b8f881        	l32r	a8, 4212d0a8 <dl_nn_get_tanh_table+0x464> (400023b8 <__ledf2>)
4213ecca:	0008e0        	callx8	a8
4213eccd:	b8fbc1        	l32r	a12, 4212d0bc <dl_nn_get_tanh_table+0x478> (80000000 <_rtc_reserved_end+0x1ff00000>)
4213ecd0:	4188      	l32i.n	a8, a1, 16
4213ecd2:	5198      	l32i.n	a9, a1, 20
4213ecd4:	101aa6        	blti	a10, 1, 4213ece8 <parse_value+0x184>
4213ecd7:	06ad      	mov.n	a10, a6
4213ecd9:	20b770        	or	a11, a7, a7
4213ecdc:	b7fa81        	l32r	a8, 4212ccc4 <dl_nn_get_tanh_table+0x80> (400022d4 <__fixdfsi>)
4213ecdf:	0008e0        	callx8	a8
4213ece2:	5198      	l32i.n	a9, a1, 20
4213ece4:	4188      	l32i.n	a8, a1, 16
4213ece6:	0acd      	mov.n	a12, a10
4213ece8:	23b8      	l32i.n	a11, a3, 8
4213ecea:	c09980        	sub	a9, a9, a8
4213eced:	8a0c      	movi.n	a10, 8
4213ecef:	32a9      	s32i.n	a10, a2, 12
4213ecf1:	6269      	s32i.n	a6, a2, 24
4213ecf3:	7279      	s32i.n	a7, a2, 28
4213ecf5:	52c9      	s32i.n	a12, a2, 20
4213ecf7:	9b9a      	add.n	a9, a11, a9
4213ecf9:	2399      	s32i.n	a9, a3, 8
4213ecfb:	08ad      	mov.n	a10, a8
4213ecfd:	0005e0        	callx8	a5
4213ed00:	120c      	movi.n	a2, 1
4213ed02:	009786        	j	4213ef64 <parse_value+0x400>
4213ed05:	ba5c00        	ceil.s	a5, f12, 0
4213ed08:	021ba7        	beq	a11, a10, 4213ed0e <parse_value+0x1aa>
4213ed0b:	003086        	j	4213edd1 <parse_value+0x26d>
4213ed0e:	33e8      	l32i.n	a14, a3, 12
4213ed10:	e7a3a2        	movi	a10, 0x3e7
4213ed13:	0e7d      	mov.n	a7, a14
4213ed15:	373ae7        	bltu	a10, a14, 4213ed50 <parse_value+0x1ec>
4213ed18:	ae1b      	addi.n	a10, a14, 1
4213ed1a:	33a9      	s32i.n	a10, a3, 12
4213ed1c:	000fa2        	l8ui	a10, a15, 0
4213ed1f:	2d9ab7        	bne	a10, a11, 4213ed50 <parse_value+0x1ec>
4213ed22:	881b      	addi.n	a8, a8, 1
4213ed24:	2389      	s32i.n	a8, a3, 8
4213ed26:	03ad      	mov.n	a10, a3
4213ed28:	4199      	s32i.n	a9, a1, 16
4213ed2a:	61c9      	s32i.n	a12, a1, 24
4213ed2c:	51d9      	s32i.n	a13, a1, 20
4213ed2e:	b8eb81        	l32r	a8, 4212d0dc <dl_nn_get_tanh_table+0x498> (42209510 <buffer_skip_whitespace>)
4213ed31:	0008e0        	callx8	a8
4213ed34:	23e8      	l32i.n	a14, a3, 8
4213ed36:	4198      	l32i.n	a9, a1, 16
4213ed38:	10be97        	bgeu	a14, a9, 4213ed4c <parse_value+0x1e8>
4213ed3b:	51d8      	l32i.n	a13, a1, 20
4213ed3d:	d85c      	movi.n	a8, 93
4213ed3f:	ddea      	add.n	a13, a13, a14
4213ed41:	000d92        	l8ui	a9, a13, 0
4213ed44:	61c8      	l32i.n	a12, a1, 24
4213ed46:	0b9987        	bne	a9, a8, 4213ed55 <parse_value+0x1f1>
4213ed49:	006c06        	j	4213eefd <parse_value+0x399>
4213ed4c:	ee0b      	addi.n	a14, a14, -1
4213ed4e:	23e9      	s32i.n	a14, a3, 8
4213ed50:	020c      	movi.n	a2, 0
4213ed52:	008386        	j	4213ef64 <parse_value+0x400>
4213ed55:	ee0b      	addi.n	a14, a14, -1
4213ed57:	0c7d      	mov.n	a7, a12
4213ed59:	23e9      	s32i.n	a14, a3, 8
4213ed5b:	0c5d      	mov.n	a5, a12
4213ed5d:	4388      	l32i.n	a8, a3, 16
4213ed5f:	8a2c      	movi.n	a10, 40
4213ed61:	0008e0        	callx8	a8
4213ed64:	076d      	mov.n	a6, a7
4213ed66:	8c2c      	movi.n	a12, 40
4213ed68:	0b0c      	movi.n	a11, 0
4213ed6a:	0a7d      	mov.n	a7, a10
4213ed6c:	16da16        	beqz	a10, 4213eedd <parse_value+0x379>
4213ed6f:	b80d81        	l32r	a8, 4212cda4 <dl_nn_get_tanh_table+0x160> (400011e8 <memset>)
4213ed72:	0008e0        	callx8	a8
4213ed75:	558c      	beqz.n	a5, 4213ed7e <parse_value+0x21a>
4213ed77:	0679      	s32i.n	a7, a6, 0
4213ed79:	1769      	s32i.n	a6, a7, 4
4213ed7b:	000046        	j	4213ed80 <parse_value+0x21c>
4213ed7e:	075d      	mov.n	a5, a7
4213ed80:	2388      	l32i.n	a8, a3, 8
4213ed82:	03ad      	mov.n	a10, a3
4213ed84:	881b      	addi.n	a8, a8, 1
4213ed86:	2389      	s32i.n	a8, a3, 8
4213ed88:	b8d581        	l32r	a8, 4212d0dc <dl_nn_get_tanh_table+0x498> (42209510 <buffer_skip_whitespace>)
4213ed8b:	0008e0        	callx8	a8
4213ed8e:	03bd      	mov.n	a11, a3
4213ed90:	07ad      	mov.n	a10, a7
4213ed92:	ffdd25        	call8	4213eb64 <parse_value>
4213ed95:	0a8d      	mov.n	a8, a10
4213ed97:	03ad      	mov.n	a10, a3
4213ed99:	143816        	beqz	a8, 4213eee0 <parse_value+0x37c>
4213ed9c:	b8d081        	l32r	a8, 4212d0dc <dl_nn_get_tanh_table+0x498> (42209510 <buffer_skip_whitespace>)
4213ed9f:	0008e0        	callx8	a8
4213eda2:	23e8      	l32i.n	a14, a3, 8
4213eda4:	1388      	l32i.n	a8, a3, 4
4213eda6:	023e87        	bltu	a14, a8, 4213edac <parse_value+0x248>
4213eda9:	004cc6        	j	4213eee0 <parse_value+0x37c>
4213edac:	0388      	l32i.n	a8, a3, 0
4213edae:	c92c      	movi.n	a9, 44
4213edb0:	88ea      	add.n	a8, a8, a14
4213edb2:	000882        	l8ui	a8, a8, 0
4213edb5:	a41897        	beq	a8, a9, 4213ed5d <parse_value+0x1f9>
4213edb8:	d95c      	movi.n	a9, 93
4213edba:	029897        	bne	a8, a9, 4213edc0 <parse_value+0x25c>
4213edbd:	004c46        	j	4213eef2 <parse_value+0x38e>
4213edc0:	004706        	j	4213eee0 <parse_value+0x37c>
4213edc3:	082c      	movi.n	a8, 32
4213edc5:	3289      	s32i.n	a8, a2, 12
4213edc7:	2259      	s32i.n	a5, a2, 8
4213edc9:	ee1b      	addi.n	a14, a14, 1
4213edcb:	004706        	j	4213eeeb <parse_value+0x387>
4213edce:	000000        	ill
4213edd1:	7ba0a2        	movi	a10, 123
4213edd4:	021ba7        	beq	a11, a10, 4213edda <parse_value+0x276>
4213edd7:	ffdd46        	j	4213ed50 <parse_value+0x1ec>
4213edda:	33c8      	l32i.n	a12, a3, 12
4213eddc:	e7a3a2        	movi	a10, 0x3e7
4213eddf:	02bac7        	bgeu	a10, a12, 4213ede5 <parse_value+0x281>
4213ede2:	ffda86        	j	4213ed50 <parse_value+0x1ec>
4213ede5:	ac1b      	addi.n	a10, a12, 1
4213ede7:	33a9      	s32i.n	a10, a3, 12
4213ede9:	000fa2        	l8ui	a10, a15, 0
4213edec:	021ab7        	beq	a10, a11, 4213edf2 <parse_value+0x28e>
4213edef:	ffd746        	j	4213ed50 <parse_value+0x1ec>
4213edf2:	881b      	addi.n	a8, a8, 1
4213edf4:	2389      	s32i.n	a8, a3, 8
4213edf6:	03ad      	mov.n	a10, a3
4213edf8:	4199      	s32i.n	a9, a1, 16
4213edfa:	61c9      	s32i.n	a12, a1, 24
4213edfc:	51d9      	s32i.n	a13, a1, 20
4213edfe:	b8b781        	l32r	a8, 4212d0dc <dl_nn_get_tanh_table+0x498> (42209510 <buffer_skip_whitespace>)
4213ee01:	0008e0        	callx8	a8
4213ee04:	23e8      	l32i.n	a14, a3, 8
4213ee06:	4198      	l32i.n	a9, a1, 16
4213ee08:	023e97        	bltu	a14, a9, 4213ee0e <parse_value+0x2aa>
4213ee0b:	ffcf46        	j	4213ed4c <parse_value+0x1e8>
4213ee0e:	51d8      	l32i.n	a13, a1, 20
4213ee10:	7da082        	movi	a8, 125
4213ee13:	ddea      	add.n	a13, a13, a14
4213ee15:	000d92        	l8ui	a9, a13, 0
4213ee18:	61c8      	l32i.n	a12, a1, 24
4213ee1a:	029987        	bne	a9, a8, 4213ee20 <parse_value+0x2bc>
4213ee1d:	003c06        	j	4213ef11 <parse_value+0x3ad>
4213ee20:	080c      	movi.n	a8, 0
4213ee22:	ee0b      	addi.n	a14, a14, -1
4213ee24:	087d      	mov.n	a7, a8
4213ee26:	23e9      	s32i.n	a14, a3, 8
4213ee28:	085d      	mov.n	a5, a8
4213ee2a:	4388      	l32i.n	a8, a3, 16
4213ee2c:	8a2c      	movi.n	a10, 40
4213ee2e:	0008e0        	callx8	a8
4213ee31:	076d      	mov.n	a6, a7
4213ee33:	0a7d      	mov.n	a7, a10
4213ee35:	0a4a16        	beqz	a10, 4213eedd <parse_value+0x379>
4213ee38:	8c2c      	movi.n	a12, 40
4213ee3a:	0b0c      	movi.n	a11, 0
4213ee3c:	b7da81        	l32r	a8, 4212cda4 <dl_nn_get_tanh_table+0x160> (400011e8 <memset>)
4213ee3f:	0008e0        	callx8	a8
4213ee42:	858c      	beqz.n	a5, 4213ee4e <parse_value+0x2ea>
4213ee44:	0679      	s32i.n	a7, a6, 0
4213ee46:	1769      	s32i.n	a6, a7, 4
4213ee48:	000106        	j	4213ee50 <parse_value+0x2ec>
4213ee4b:	000000        	ill
4213ee4e:	075d      	mov.n	a5, a7
4213ee50:	2388      	l32i.n	a8, a3, 8
4213ee52:	1398      	l32i.n	a9, a3, 4
4213ee54:	881b      	addi.n	a8, a8, 1
4213ee56:	023897        	bltu	a8, a9, 4213ee5c <parse_value+0x2f8>
4213ee59:	0020c6        	j	4213eee0 <parse_value+0x37c>
4213ee5c:	2389      	s32i.n	a8, a3, 8
4213ee5e:	03ad      	mov.n	a10, a3
4213ee60:	b89f81        	l32r	a8, 4212d0dc <dl_nn_get_tanh_table+0x498> (42209510 <buffer_skip_whitespace>)
4213ee63:	0008e0        	callx8	a8
4213ee66:	03bd      	mov.n	a11, a3
4213ee68:	07ad      	mov.n	a10, a7
4213ee6a:	ff1d25        	call8	4213e03c <parse_string>
4213ee6d:	06fa16        	beqz	a10, 4213eee0 <parse_value+0x37c>
4213ee70:	03ad      	mov.n	a10, a3
4213ee72:	b89a81        	l32r	a8, 4212d0dc <dl_nn_get_tanh_table+0x498> (42209510 <buffer_skip_whitespace>)
4213ee75:	0008e0        	callx8	a8
4213ee78:	47a8      	l32i.n	a10, a7, 16
4213ee7a:	2388      	l32i.n	a8, a3, 8
4213ee7c:	1398      	l32i.n	a9, a3, 4
4213ee7e:	87a9      	s32i.n	a10, a7, 32
4213ee80:	0a0c      	movi.n	a10, 0
4213ee82:	47a9      	s32i.n	a10, a7, 16
4213ee84:	58b897        	bgeu	a8, a9, 4213eee0 <parse_value+0x37c>
4213ee87:	0398      	l32i.n	a9, a3, 0
4213ee89:	aa3c      	movi.n	a10, 58
4213ee8b:	998a      	add.n	a9, a9, a8
4213ee8d:	000992        	l8ui	a9, a9, 0
4213ee90:	4c99a7        	bne	a9, a10, 4213eee0 <parse_value+0x37c>
4213ee93:	881b      	addi.n	a8, a8, 1
4213ee95:	026382        	s32i	a8, a3, 8
4213ee98:	20a330        	or	a10, a3, a3
4213ee9b:	b89081        	l32r	a8, 4212d0dc <dl_nn_get_tanh_table+0x498> (42209510 <buffer_skip_whitespace>)
4213ee9e:	0008e0        	callx8	a8
4213eea1:	03bd      	mov.n	a11, a3
4213eea3:	07ad      	mov.n	a10, a7
4213eea5:	ffcbe5        	call8	4213eb64 <parse_value>
4213eea8:	4abc      	beqz.n	a10, 4213eee0 <parse_value+0x37c>
4213eeaa:	03ad      	mov.n	a10, a3
4213eeac:	b88c81        	l32r	a8, 4212d0dc <dl_nn_get_tanh_table+0x498> (42209510 <buffer_skip_whitespace>)
4213eeaf:	0008e0        	callx8	a8
4213eeb2:	23e8      	l32i.n	a14, a3, 8
4213eeb4:	1388      	l32i.n	a8, a3, 4
4213eeb6:	26be87        	bgeu	a14, a8, 4213eee0 <parse_value+0x37c>
4213eeb9:	0388      	l32i.n	a8, a3, 0
4213eebb:	c92c      	movi.n	a9, 44
4213eebd:	88ea      	add.n	a8, a8, a14
4213eebf:	000882        	l8ui	a8, a8, 0
4213eec2:	029897        	bne	a8, a9, 4213eec8 <parse_value+0x364>
4213eec5:	ffd846        	j	4213ee2a <parse_value+0x2c6>
4213eec8:	7da092        	movi	a9, 125
4213eecb:	351897        	beq	a8, a9, 4213ef04 <parse_value+0x3a0>
4213eece:	000386        	j	4213eee0 <parse_value+0x37c>
4213eed1:	084c      	movi.n	a8, 64
4213eed3:	3289      	s32i.n	a8, a2, 12
4213eed5:	2259      	s32i.n	a5, a2, 8
4213eed7:	ee1b      	addi.n	a14, a14, 1
4213eed9:	000386        	j	4213eeeb <parse_value+0x387>
4213eedc:	f51600        	extui	a1, a0, 22, 16
4213eedf:	05ade6        	bgei	a13, 12, 4213eee8 <parse_value+0x384>
4213eee2:	ffc1a5        	call8	4213eafc <cJSON_Delete>
4213eee5:	ff99c6        	j	4213ed50 <parse_value+0x1ec>
4213eee8:	000000        	ill
4213eeeb:	23e9      	s32i.n	a14, a3, 8
4213eeed:	120c      	movi.n	a2, 1
4213eeef:	001c46        	j	4213ef64 <parse_value+0x400>
4213eef2:	3388      	l32i.n	a8, a3, 12
4213eef4:	880b      	addi.n	a8, a8, -1
4213eef6:	3389      	s32i.n	a8, a3, 12
4213eef8:	1579      	s32i.n	a7, a5, 4
4213eefa:	ffb146        	j	4213edc3 <parse_value+0x25f>
4213eefd:	3379      	s32i.n	a7, a3, 12
4213eeff:	0c5d      	mov.n	a5, a12
4213ef01:	ffaf86        	j	4213edc3 <parse_value+0x25f>
4213ef04:	3388      	l32i.n	a8, a3, 12
4213ef06:	880b      	addi.n	a8, a8, -1
4213ef08:	3389      	s32i.n	a8, a3, 12
4213ef0a:	1579      	s32i.n	a7, a5, 4
4213ef0c:	fff046        	j	4213eed1 <parse_value+0x36d>
4213ef0f:	00          	.byte	00
4213ef10:	33c900        	clamps	a12, a9, 7
4213ef13:	050c      	movi.n	a5, 0
4213ef15:	ffee06        	j	4213eed1 <parse_value+0x36d>
4213ef18:	b85bb1        	l32r	a11, 4212d084 <dl_nn_get_tanh_table+0x440> (3c26b1a8 <_flash_rodata_start+0x4b088>)
4213ef1b:	ad8a      	add.n	a10, a13, a8
4213ef1d:	5c0c      	movi.n	a12, 5
4213ef1f:	71e9      	s32i.n	a14, a1, 28
4213ef21:	6199      	s32i.n	a9, a1, 24
4213ef23:	5189      	s32i.n	a8, a1, 20
4213ef25:	41d9      	s32i.n	a13, a1, 16
4213ef27:	b86a81        	l32r	a8, 4212d0d0 <dl_nn_get_tanh_table+0x48c> (4000123c <strncmp>)
4213ef2a:	0008e0        	callx8	a8
4213ef2d:	41d8      	l32i.n	a13, a1, 16
4213ef2f:	5188      	l32i.n	a8, a1, 20
4213ef31:	6198      	l32i.n	a9, a1, 24
4213ef33:	71e8      	l32i.n	a14, a1, 28
4213ef35:	c81a16        	beqz	a10, 4213ebba <parse_value+0x56>
4213ef38:	ff2c06        	j	4213ebec <parse_value+0x88>
4213ef3b:	b10000        	sra	a0, a0
4213ef3e:	0cb851        	l32r	a5, 42102220 <rgb888_image_blend+0x6bc> (c2aa80c0 <_rtc_reserved_end+0x629a80c0>)
4213ef41:	ad5c      	movi.n	a13, 90
4213ef43:	81e907        	bbsi	a9, 0, 4213eec8 <parse_value+0x364>
4213ef46:	71f9      	s32i.n	a15, a1, 28
4213ef48:	6199      	s32i.n	a9, a1, 24
4213ef4a:	5189      	s32i.n	a8, a1, 20
4213ef4c:	41d9      	s32i.n	a13, a1, 16
4213ef4e:	b86081        	l32r	a8, 4212d0d0 <dl_nn_get_tanh_table+0x48c> (4000123c <strncmp>)
4213ef51:	0008e0        	callx8	a8
4213ef54:	41d8      	l32i.n	a13, a1, 16
4213ef56:	5188      	l32i.n	a8, a1, 20
4213ef58:	6198      	l32i.n	a9, a1, 24
4213ef5a:	71f8      	l32i.n	a15, a1, 28
4213ef5c:	81e8      	l32i.n	a14, a1, 32
4213ef5e:	c5fa56        	bnez	a10, 4213ebc1 <parse_value+0x5d>
4213ef61:	ff14c6        	j	4213ebb8 <parse_value+0x54>
4213ef64:	f01d      	retw.n
