#!/usr/bin/env python3

"""Helper script to generate images which contain LWM2M metadata.
Use this script to generate a binary which contains the LWM2M device ID
and device token. (run with -h for usage).
Then flash a board as follows:
1. Flash mcuboot, performing a chip erase
2. Flash the LWM2M credentials binary
3. Flash the FOTA application
Now you can perform dual-bank FOTA updates without changing the LWM2M
credentials binary. The configuration data there will persist.
Here is an example for Nitrogen:
    pyocd-flashtool -b $board -t nrf52 -ce -a 0x0 "$mcuboot_image"
    pyocd-flashtool -b $board -t nrf52 -a 0x7f000 "$credentials_image"
    pyocd-flashtool -b $board -t nrf52 -a 0x8000 "$app_image"
Using the -b argument allows you to flash different boards with different
LWM2M credentials binaries.
Refer to the value FLASH_AREA_CREDENTIALS_STATE_OFFSET in the Genesis build
output file outdir/$APP/$BOARD/app/include/generated/generated_dts_board.h for
your board for the base address of the LWM2M credentials partition."""


import argparse
import sys

LWM2M_DEVICE_ID_SIZE = 32 + 1
LWM2M_DEVICE_TOKEN_SIZE = 32 + 1


def write_state(device_id, device_token, out):
    state = (bytearray(device_id) + bytearray([0x00]) +
             bytearray(device_token) + bytearray([0x00]))
    out.write(state)


def main():
    parser = argparse.ArgumentParser(
        description='''Generate a binary flashable
                    as an LWM2M credentials partition.''')

    parser.add_argument('-did', '--device-id',
                        required=True, help='Device unique ID')
    parser.add_argument('-dtok', '--device-token', default='',
                        required=False, help='Device token')
    parser.add_argument('-o', '--output',
                        default=sys.stdout,
                        help='Output file (default: stdout)')

    args = parser.parse_args(sys.argv[1:])

    if len(args.device_id) > LWM2M_DEVICE_ID_SIZE - 1:
        print('Invalid device ID (length should be up to ' +
              str(LWM2M_DEVICE_ID_SIZE - 1) + ')', file=sys.stderr)
        parser.print_help()
        sys.exit(1)

    if len(args.device_token) > LWM2M_DEVICE_TOKEN_SIZE - 1:
        print('Invalid device token (length should be up to ' +
              str(LWM2M_DEVICE_TOKEN_SIZE - 1) + ')', file=sys.stderr)
        parser.print_help()
        sys.exit(1)

    did, dtok = (x.ljust(32, '\0').encode('ascii') for x in
                       (args.device_id, args.device_token))

    if args.output is sys.stdout:
        write_state(did, dtok, sys.stdout.buffer)
    else:
        with open(args.output, 'wb') as out:
            write_state(did, dtok, out)


if __name__ == '__main__':
    main()
