### Gameboy RA Adapter PoC

## Proof of Concept (PoC)

This PoC, developed in a single day, demonstrates that a significant portion of the NES RA Adapter can be repurposed for another console.

The rcheevos integration, user interface, internet connectivity—everything can be reused. It will automatically fetch the game image and achievements regardless of the console.

GB is similar to NES, so this was a easy PoC. The adapter intercepts easily the memory writes on the working ram (address from 0xC000-0xDFFF). The raspberry pico has all the ports to handle GB signals.

Although the PoC worked, it still has some issues we will need to work out in the future. The main problem is finding a way to inspect the High RAM, that is located inside the CPU SoC. We would also need to remove the LCD screen to save energy and use a power supply.

To speed things up I did't implement the game identification process - This would demand a complex circuit to control the bus. That's why the game identification is hardcoded into the ESP32 firmware.

gb-esp-firmware and gb-pico-firmware are forks from the respective repositories of NES at 2025-03-07

## Successful Tests:

- Successfully triggered an achievement in Zelda Link's Awekening [https://youtu.be/6lw4mGO8Hv8](https://youtu.be/6lw4mGO8Hv8)

## Conclusion

Adapting the NES RA Adapter for preliminary GB testing was straightforward and it capture memory writes on working ram in a very robust way. The challenging will be capture writes on high ram. 
