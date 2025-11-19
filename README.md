PowerHAL SoCDaemon v0.72
=========================

How to build SoCDaemon
=========================
1.Copy /ptl_init_socdaemon to your /vendor/intel/socdaemon directory.

2.Apply the patch /socdaemon/platform_patch/0001-device_google_desktop_common*.patch to /device/google/desktop/common.

3.Apply the patch /socdaemon/platform_patch/0001-device_google_desktop_fatcat*.patch to /device/google/desktop/fatcat.

4.Proceed with your standard AOSP build process.


Caution
=========================
1.There is currently no official PTL-404, so you must manually set the powerhint.json file on every boot.

2.Use the following commands to set the appropriate configuration:

3.setprop vendor.powerhal.config power/powerhint_484.json for 484 Silicon

4.setprop vendor.powerhal.config power/powerhint_204.json for 204 Silicon


Running SoCDaemon
=========================
1.Flash your build to the Device Under Test (DUT).

2.Use /vendor/bin/socdaemon --help for guidance, or refer to the typical commands below:

3./vendor/bin/socdaemon --sendHint false //This command will not trigger core containment, but allows you to check the socHints.

4./vendor/bin/socdaemon --sendHint true //This command triggers core containment using WLT by default.

5./vendor/bin/socdaemon --sendHint true --sochint wlt //Enables WLT-based core containment.

6./vendor/bin/socdaemon --sendHint true --sochint swlt //Enables Slow WLT-based core containment.

7./vendor/bin/socdaemon --sendHint true --sochint hfi //Enables HFI-based core containment.

8./vendor/bin/socdaemon --sendHint true --sochint wlt --notification_delay 512 //Enables WLT-based core containment with a notification delay.
