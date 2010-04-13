using System;
using System.IO;
using System.Collections.Generic;
using System.Text;

using SysGen.RBuild.Framework;

namespace SysGen.BuildEngine.Framework
{
    public class UnAttendSetupFileWriter : AutoGeneratedInfFileWriter
    {
        public UnAttendSetupFileWriter(RBuildProject project, string file)
            : base(project , file)
        {
        }

        public override void WriteFile()
        {
            WriteHeader();
            WriteUnAttendFile();
        }

        protected override void WriteHeader()
        {
            WriteSection("Unattend");
            WriteLine("Signature = \"$ReactOS$\"");
            WriteLine();
        }

        protected void WriteUnAttendFile()
        {
            WriteBooleanAssignment("UnattendSetupEnabled", false);
            WriteAssignment("DestinationDiskNumber", "0");
            WriteAssignment("DestinationPartitionNumber", "1");
            WriteAssignment("MBRInstallType", "2");
            WriteAssignment("FullName", "MyName");
            WriteAssignment("OrgName", "MyOrg");
            WriteAssignment("ComputerName", "MyComputer");
            WriteAssignment("AdminPassword", "MyPassword");
            WriteAssignment("TimeZoneIndex", "85");
            WriteAssignment("FormatPartition", "1");
            WriteAssignment("AutoPartition", "1");
            WriteAssignment("DisableVmwInst", "1");
        }
    }
}
