local function greeting()
  local name = get_config("name")
  if name == "" then
    name = "FinaSU"
  end
  return "Hello, " .. name .. "!\n"
end

return {
  service = function()
    assert(write_file(PLUGIN_DIR .. "/data/greeting.txt", greeting()))
    info("greeting updated")
  end,

  action = function()
    assert(write_file(PLUGIN_DIR .. "/data/greeting.txt", greeting()))
  end,
}
