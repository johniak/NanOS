;=============================================================================
; ATA read sectors (LBA mode)
;
; @param EAX Logical Block Address of sector
; @param CL  Number of sectors to read
; @param EDI The address of buffer to put data obtained from disk
;
; @return EAX 0 on success, 1 on error (ERR/DF status bit set, or DRQ timed out)
;=============================================================================
ata_lba_read:
               pushf
               push ebx
               push ecx
               push edx
               push edi
               push esi
               ; NOTE: EAX is NOT preserved -- it carries the LBA in, the status out.

               mov ebx, eax         ; Save LBA in EBX

               mov edx, 0x01F6      ; Port to send drive and bit 24 - 27 of LBA
               shr eax, 24          ; Get bit 24 - 27 in al
               or al, 11100000b     ; Set bit 6 in al for LBA mode
               out dx, al

               mov edx, 0x01F2      ; Port to send number of sectors
               mov al, cl           ; Get number of sectors from CL
               out dx, al

               mov edx, 0x1F3       ; Port to send bit 0 - 7 of LBA
               mov eax, ebx         ; Get LBA from EBX
               out dx, al

               mov edx, 0x1F4       ; Port to send bit 8 - 15 of LBA
               mov eax, ebx         ; Get LBA from EBX
               shr eax, 8           ; Get bit 8 - 15 in AL
               out dx, al


               mov edx, 0x1F5       ; Port to send bit 16 - 23 of LBA
               mov eax, ebx         ; Get LBA from EBX
               shr eax, 16          ; Get bit 16 - 23 in AL
               out dx, al

               mov edx, 0x1F7       ; Command port
               mov al, 0x20         ; Read with retry.
               out dx, al

               mov esi, 0x100000    ; DRQ spin timeout (guards against a stuck/absent drive)
.rd_wait:      in al, dx            ; read the status register (0x1F7)
               test al, 0x21        ; ERR (bit 0) or DF (bit 5) -> the command failed
               jnz .rd_err
               test al, 8           ; DRQ (bit 3): the sector buffer is ready to service
               jnz .rd_ready
               dec esi
               jnz .rd_wait
               jmp .rd_err          ; timed out waiting for DRQ

.rd_ready:     mov eax, 256         ; to read 256 words = 1 sector
               xor bx, bx
               mov bl, cl           ; read CL sectors
               mul bx
               mov ecx, eax         ; ECX is counter for INSW
               mov edx, 0x1F0       ; Data port, in and out
               rep insw             ; in to [EDI]
               xor eax, eax         ; success -> 0
               jmp .rd_done
.rd_err:       mov eax, 1           ; error -> 1
.rd_done:
               pop esi
               pop edi
               pop edx
               pop ecx
               pop ebx
               popf
               ret


;=============================================================================
; ATA write sectors (LBA mode)
;
; @param EAX Logical Block Address of sector
; @param CL  Number of sectors to write
; @param ESI The address of buffer to write to disk
;
; @return EAX 0 on success, 1 on error
;=============================================================================
ata_lba_write:
               pushf
               push ebx
               push ecx
               push edx
               push esi
               push edi
               ; NOTE: EAX is NOT preserved -- LBA in, status out.

               mov ebx, eax         ; Save LBA in EBX

               mov edx, 0x01F6      ; Port to send drive and bit 24 - 27 of LBA
               shr eax, 24          ; Get bit 24 - 27 in al
               or al, 11100000b     ; Set bit 6 in al for LBA mode
               out dx, al

               mov edx, 0x01F2      ; Port to send number of sectors
               mov al, cl           ; Get number of sectors from CL
               out dx, al

               mov edx, 0x1F3       ; Port to send bit 0 - 7 of LBA
               mov eax, ebx         ; Get LBA from EBX
               out dx, al

               mov edx, 0x1F4       ; Port to send bit 8 - 15 of LBA
               mov eax, ebx         ; Get LBA from EBX
               shr eax, 8           ; Get bit 8 - 15 in AL
               out dx, al


               mov edx, 0x1F5       ; Port to send bit 16 - 23 of LBA
               mov eax, ebx         ; Get LBA from EBX
               shr eax, 16          ; Get bit 16 - 23 in AL
               out dx, al

               mov edx, 0x1F7       ; Command port
               mov al, 0x30         ; Write with retry.
               out dx, al

               mov edi, 0x100000    ; DRQ spin timeout
.wr_wait:      in al, dx            ; status register (0x1F7)
               test al, 0x21        ; ERR or DF -> failed
               jnz .wr_err
               test al, 8           ; DRQ: ready for the data
               jnz .wr_ready
               dec edi
               jnz .wr_wait
               jmp .wr_err

.wr_ready:     mov eax, 256         ; 256 words = 1 sector
               xor bx, bx
               mov bl, cl           ; write CL sectors
               mul bx
               mov ecx, eax         ; ECX is counter for OUTSW
               mov edx, 0x1F0       ; Data port
               rep outsw            ; out from [ESI]
               xor eax, eax         ; success -> 0
               jmp .wr_done
.wr_err:       mov eax, 1           ; error -> 1
.wr_done:
               pop edi
               pop esi
               pop edx
               pop ecx
               pop ebx
               popf
               ret

;=============================================================================
; ATA read sectors (LBA mode), cdecl wrapper.
;
; extern "C" int read_ata_pio(int lba, char sect_count, char* buf);
;   returns 0 on success, non-zero on I/O error.
;=============================================================================
global read_ata_pio
read_ata_pio:
            pushf
            push ebp
            mov ebp, esp

            mov edi,[ebp+20]		;buff
            mov cl,[ebp+16]			;sectors count
            mov eax,[ebp+12]		;lba

            call ata_lba_read       ; EAX = status (0 ok, 1 error)

            mov esp,ebp
            pop ebp
            popf
            ret                     ; status returned in EAX

;=============================================================================
; ATA write sectors (LBA mode), cdecl wrapper.
;
; extern "C" int write_ata_pio(int lba, char sect_count, char* buf);
;   returns 0 on success, non-zero on I/O error.
;=============================================================================
global write_ata_pio
write_ata_pio:
            pushf
            push ebp
            mov ebp, esp

            mov esi,[ebp+20]		;buff
            mov cl,[ebp+16] 		;sectors count
            mov eax,[ebp+12]		;lba

            call ata_lba_write      ; EAX = status

            mov esp,ebp
            pop ebp
            popf
            ret
