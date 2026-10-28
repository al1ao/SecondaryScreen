// Windows device installation APIs only; no display driver implementation.
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;
namespace PadScreen {
    public static class VddDeviceInstaller {
        static readonly Guid DisplayClass = new Guid("4d36e968-e325-11ce-bfc1-08002be10318");
        [StructLayout(LayoutKind.Sequential)] struct DeviceInfo {
            public uint Size;
            public Guid ClassGuid;
            public uint DevInst;
            public IntPtr Reserved;
        }
        [DllImport("setupapi.dll", SetLastError=true)] static extern IntPtr SetupDiCreateDeviceInfoList(ref Guid classGuid, IntPtr parent);
        [DllImport("setupapi.dll", CharSet=CharSet.Unicode, SetLastError=true)]
        static extern bool SetupDiCreateDeviceInfoW(IntPtr set, string name, ref Guid classGuid, string description, IntPtr parent, uint flags, ref DeviceInfo data);
        [DllImport("setupapi.dll", CharSet=CharSet.Unicode, SetLastError=true)]
        static extern bool SetupDiOpenDeviceInfoW(IntPtr set, string instance, IntPtr parent, uint flags, ref DeviceInfo data);
        [DllImport("setupapi.dll", CharSet=CharSet.Unicode, SetLastError=true)]
        static extern bool SetupDiSetDeviceRegistryPropertyW(IntPtr set, ref DeviceInfo data, uint property, byte[] value, uint size);
        [DllImport("setupapi.dll", SetLastError=true)] static extern bool SetupDiCallClassInstaller(uint operation, IntPtr set, ref DeviceInfo data);
        [DllImport("setupapi.dll", CharSet=CharSet.Unicode, SetLastError=true)]
        static extern bool SetupDiGetDeviceInstanceIdW(IntPtr set, ref DeviceInfo data, StringBuilder value, uint size, out uint required);
        [DllImport("setupapi.dll", SetLastError=true)] static extern bool SetupDiDestroyDeviceInfoList(IntPtr set);
        [DllImport("newdev.dll", CharSet=CharSet.Unicode, SetLastError=true)]
        static extern bool UpdateDriverForPlugAndPlayDevicesW(IntPtr parent, string hardwareId, string inf, uint flags, out bool reboot);
        static void Check(bool ok, string operation) {
            if (!ok) throw new Win32Exception(Marshal.GetLastWin32Error(), operation);
        }
        public sealed class InstallResult {
            public string InstanceId;
            public bool RebootRequired;
        }
        public static InstallResult Install(string inf) {
            Guid guid = DisplayClass;
            IntPtr set = SetupDiCreateDeviceInfoList(ref guid, IntPtr.Zero);
            if (set == new IntPtr(-1)) throw new Win32Exception(Marshal.GetLastWin32Error());
            DeviceInfo data = new DeviceInfo { Size = (uint)Marshal.SizeOf(typeof(DeviceInfo)) };
            bool registered = false;
            try {
                Check(SetupDiCreateDeviceInfoW(set, "Display", ref guid, "Virtual Display Driver", IntPtr.Zero, 1, ref data), "Create root display device");
                byte[] hardwareId = Encoding.Unicode.GetBytes("Root\\MttVDD\0\0");
                Check(SetupDiSetDeviceRegistryPropertyW(set, ref data, 1, hardwareId, (uint)hardwareId.Length), "Set VDD hardware ID");
                Check(SetupDiCallClassInstaller(0x19, set, ref data), "Register VDD root device");
                registered = true;
                bool reboot;
                Check(UpdateDriverForPlugAndPlayDevicesW(IntPtr.Zero, "Root\\MttVDD", inf, 1, out reboot), "Install signed MttVDD package");
                StringBuilder instance = new StringBuilder(1024);
                uint required;
                Check(SetupDiGetDeviceInstanceIdW(set, ref data, instance, (uint)instance.Capacity, out required), "Read installed VDD instance ID");
                return new InstallResult { InstanceId = instance.ToString(), RebootRequired = reboot };
            } catch (Exception original) {
                // Remove only the just-created instance; never remove other display adapters.
                if (registered && !SetupDiCallClassInstaller(5, set, ref data))
                    throw new InvalidOperationException("Install failed and new VDD device rollback failed (Win32 " +
                        Marshal.GetLastWin32Error() + "). Inspect Device Manager before retrying. Original error: " + original.Message, original);
                throw;
            } finally { SetupDiDestroyDeviceInfoList(set); }
        }
        public static void Remove(string instance) {
            Guid guid = DisplayClass;
            IntPtr set = SetupDiCreateDeviceInfoList(ref guid, IntPtr.Zero);
            if (set == new IntPtr(-1)) throw new Win32Exception(Marshal.GetLastWin32Error());
            try {
                DeviceInfo data = new DeviceInfo { Size = (uint)Marshal.SizeOf(typeof(DeviceInfo)) };
                Check(SetupDiOpenDeviceInfoW(set, instance, IntPtr.Zero, 0, ref data), "Open recorded VDD device");
                Check(SetupDiCallClassInstaller(5, set, ref data), "Remove recorded VDD device");
            } finally { SetupDiDestroyDeviceInfoList(set); }
        }
    }
}
