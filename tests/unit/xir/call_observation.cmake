

foreach(direction vm-native native-vm)
    string(REPLACE "-" "_" name "${direction}")
endforeach()
