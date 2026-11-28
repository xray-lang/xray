local parts = {}
for i = 1, @N@ do parts[#parts + 1] = "ab" end
print(#table.concat(parts))
