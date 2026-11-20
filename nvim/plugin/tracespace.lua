-- Defines :Tracespace even when setup() was never called (defaults apply).
if vim.g.loaded_tracespace then
  return
end
vim.g.loaded_tracespace = true

if vim.fn.exists(":Tracespace") == 0 then
  vim.api.nvim_create_user_command("Tracespace", function()
    require("tracespace").toggle()
  end, { desc = "Toggle the tracespace execution-flow diagram" })
end
