" NanOS minimal $VIMRUNTIME/defaults.vim.
"
" The full vim runtime is ~30 MB and does not fit the NanOS disk image, so we ship just this
" file. vim sources it when there is no user vimrc; providing it removes the "E1187: Failed to
" source defaults.vim" prompt. It sets sane defaults but deliberately does NOT enable syntax or
" filetype detection, since those need the (absent) syntax/ and ftplugin/ runtime directories.
set nocompatible
set backspace=indent,eol,start
set history=200
set ruler
set showcmd
set wildmenu
set incsearch
set hlsearch
set display=truncate
set scrolloff=5
set ttimeout
set ttimeoutlen=100
