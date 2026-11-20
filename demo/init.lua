-- Minimal Neovim config for the demo recording: loads tracespace.nvim from this
-- repository and nothing else.
--   nvim -u demo/init.lua examples/basics.cpp
local root = vim.fn.fnamemodify(debug.getinfo(1, "S").source:sub(2), ":p:h:h")
vim.opt.runtimepath:append(root .. "/nvim")

vim.o.number = true
vim.o.swapfile = false
vim.o.autoindent = true
vim.o.expandtab = true
vim.o.shiftwidth = 4
vim.o.termguicolors = true
vim.o.wrapscan = true
vim.o.hlsearch = false
vim.o.showmode = false

require("tracespace").setup({
  binary = vim.env.TRACESPACE_BIN or "tracespace",
  width = 64,
})
