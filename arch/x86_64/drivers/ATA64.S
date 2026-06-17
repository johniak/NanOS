;=============================================================================
; arch/x86_64/drivers/ATA64.S — ATA PIO read/write (LBA28), long-mode port.
;
; Port I/O (in/out) is IDENTICAL in long mode. The only real changes vs the
; i686 arch/x86/drivers/ATA.S are:
;   (1) the data buffer pointer is a 64-bit register — RDI on the read path,
;       RSI on the write path — because `rep insw`/`rep outsw` use ES:[RDI] /
;       DS:[RSI] with 64-bit address size in long mode;
;   (2) flags + saved regs are pushed 64-bit (pushfq / push rbx ...);
;   (3) the cdecl stack-arg wrappers become System V AMD64 register-arg
;       wrappers: lba in EDI, sect_count in SIL, buf in RDX.
; LBA is 28-bit so it still lives in a 32-bit register (EAX/EBX). The caller
; (AtaBlockDevice64) issues ONE command per sector — the i686 `rep insw`
; multi-sector desync quirk is unchanged in long mode.
;=============================================================================

bits 64
section .text

;-----------------------------------------------------------------------------
; ata_lba_read — internal.
;   EAX = LBA (28-bit), CL = sector count, RDI = destination buffer.
;   returns EAX = 0 on success, 1 on error (ERR/DF set, or DRQ timed out).
;   NOTE: EAX is NOT preserved — LBA in, status out.
;-----------------------------------------------------------------------------
ata_lba_read:
               pushfq
               push rbx
               push rcx
               push rdx
               push rdi
               push rsi

               mov ebx, eax         ; save LBA in EBX (28-bit, fits 32)

               mov edx, 0x01F6      ; drive / LBA bits 24-27
               shr eax, 24
               or  al, 11100000b    ; LBA mode, master
               out dx, al

               mov edx, 0x01F2      ; sector count
               mov al, cl
               out dx, al

               mov edx, 0x1F3       ; LBA bits 0-7
               mov eax, ebx
               out dx, al

               mov edx, 0x1F4       ; LBA bits 8-15
               mov eax, ebx
               shr eax, 8
               out dx, al

               mov edx, 0x1F5       ; LBA bits 16-23
               mov eax, ebx
               shr eax, 16
               out dx, al

               mov edx, 0x1F7       ; command port
               mov al, 0x20         ; READ SECTORS (with retry)
               out dx, al

               mov esi, 0x100000    ; DRQ spin timeout (RSI is free on the read path)
.rd_wait:      in  al, dx           ; status register (0x1F7)
               test al, 0x21        ; ERR (bit0) or DF (bit5) -> the command failed
               jnz .rd_err
               test al, 8           ; DRQ (bit3) -> sector buffer ready
               jnz .rd_ready
               dec esi
               jnz .rd_wait
               jmp .rd_err          ; timed out waiting for DRQ

.rd_ready:     mov eax, 256         ; 256 words = 1 sector
               xor bx, bx
               mov bl, cl
               mul bx               ; DX:AX = 256 * count (count==1 -> 256)
               mov ecx, eax         ; rep counter (zero-extends into RCX)
               mov edx, 0x1F0       ; data port
               rep insw             ; words -> ES:[RDI]; RDI is the 64-bit buffer
               xor eax, eax         ; success -> 0
               jmp .rd_done
.rd_err:       mov eax, 1           ; error -> 1
.rd_done:
               pop rsi
               pop rdi
               pop rdx
               pop rcx
               pop rbx
               popfq
               ret

;-----------------------------------------------------------------------------
; ata_lba_write — internal.
;   EAX = LBA (28-bit), CL = sector count, RSI = source buffer.
;   returns EAX = 0 on success, 1 on error.
;-----------------------------------------------------------------------------
ata_lba_write:
               pushfq
               push rbx
               push rcx
               push rdx
               push rsi
               push rdi

               mov ebx, eax         ; save LBA in EBX

               mov edx, 0x01F6      ; drive / LBA bits 24-27
               shr eax, 24
               or  al, 11100000b
               out dx, al

               mov edx, 0x01F2      ; sector count
               mov al, cl
               out dx, al

               mov edx, 0x1F3       ; LBA bits 0-7
               mov eax, ebx
               out dx, al

               mov edx, 0x1F4       ; LBA bits 8-15
               mov eax, ebx
               shr eax, 8
               out dx, al

               mov edx, 0x1F5       ; LBA bits 16-23
               mov eax, ebx
               shr eax, 16
               out dx, al

               mov edx, 0x1F7       ; command port
               mov al, 0x30         ; WRITE SECTORS (with retry)
               out dx, al

               mov edi, 0x100000    ; DRQ spin timeout (EDI is free on the write path)
.wr_wait:      in  al, dx           ; status register (0x1F7)
               test al, 0x21        ; ERR or DF -> failed
               jnz .wr_err
               test al, 8           ; DRQ -> ready for the data
               jnz .wr_ready
               dec edi
               jnz .wr_wait
               jmp .wr_err

.wr_ready:     mov eax, 256         ; 256 words = 1 sector
               xor bx, bx
               mov bl, cl
               mul bx
               mov ecx, eax         ; rep counter
               mov edx, 0x1F0       ; data port
               rep outsw            ; words from DS:[RSI]; RSI is the 64-bit buffer
               ; The drive is BSY committing the sector. Wait for it to clear before the
               ; next command, or back-to-back single-sector writes race the busy drive.
               mov edx, 0x1F7
               mov edi, 0x100000
.wr_drain:     in  al, dx
               test al, 0x21        ; ERR or DF -> failed
               jnz .wr_err
               test al, 0x80        ; BSY still set?
               jz  .wr_flush
               dec edi
               jnz .wr_drain
               jmp .wr_err
               ; FLUSH CACHE: force the write to the medium so it survives reboot/power loss.
.wr_flush:     mov al, 0xE7
               out dx, al
               mov edi, 0x100000
.wr_fwait:     in  al, dx
               test al, 0x80        ; BSY clear after the flush?
               jz  .wr_ok
               dec edi
               jnz .wr_fwait
.wr_ok:        xor eax, eax         ; success -> 0
               jmp .wr_done
.wr_err:       mov eax, 1           ; error -> 1
.wr_done:
               pop rdi
               pop rsi
               pop rdx
               pop rcx
               pop rbx
               popfq
               ret

;=============================================================================
; extern "C" int read_ata_pio(int lba, char sect_count, char* buf);
;   System V AMD64: lba in EDI, sect_count in SIL, buf in RDX. Returns EAX.
;=============================================================================
global read_ata_pio
read_ata_pio:
               push rbp
               mov  rbp, rsp
               mov  eax, edi        ; LBA (read before clobbering RDI)
               mov  cl,  sil        ; sector count
               mov  rdi, rdx        ; destination buffer (full 64-bit pointer)
               call ata_lba_read    ; EAX = status (0 ok, 1 error)
               pop  rbp
               ret                  ; status in EAX

;=============================================================================
; extern "C" int write_ata_pio(int lba, char sect_count, char* buf);
;   System V AMD64: lba in EDI, sect_count in SIL, buf in RDX. Returns EAX.
;=============================================================================
global write_ata_pio
write_ata_pio:
               push rbp
               mov  rbp, rsp
               mov  eax, edi        ; LBA
               mov  cl,  sil        ; sector count (read SIL before clobbering RSI)
               mov  rsi, rdx        ; source buffer (RSI for rep outsw)
               call ata_lba_write   ; EAX = status
               pop  rbp
               ret
