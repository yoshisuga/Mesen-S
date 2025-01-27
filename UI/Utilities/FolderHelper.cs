﻿using Mesen.GUI.Config;
using Mesen.GUI.Forms.Config;
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace Mesen.GUI.Utilities
{
	public static class FolderHelper
	{
		public static bool CheckFolderPermissions(string folder, bool checkWritePermission = true)
		{
			if(!Directory.Exists(folder)) {
				try {
					if(string.IsNullOrWhiteSpace(folder)) {
						return false;
					}
					Directory.CreateDirectory(folder);
				} catch {
					return false;
				}
			}
			if(checkWritePermission) {
				try {
					File.WriteAllText(Path.Combine(folder, "test.txt"), "");
					File.Delete(Path.Combine(folder, "test.txt"));
				} catch {
					return false;
				}
			}
			return true;
		}
		
		public static bool MigrateData(string source, string target, Form parent)
		{
			using(frmCopyFiles frm = new frmCopyFiles(source, target)) {
				frm.ShowDialog(parent);
				if(frm.Exception != null) {
					throw frm.Exception;
				}
			}
			if(File.Exists(Path.Combine(source, "settings.backup.xml"))) {
				File.Delete(Path.Combine(source, "settings.backup.xml"));
			}
			File.Move(Path.Combine(source, "settings.xml"), Path.Combine(source, "settings.backup.xml"));

			ConfigManager.InitHomeFolder();
			ConfigManager.ApplyChanges();
			ConfigManager.SaveConfig();

			ConfigManager.RestartMesen(true);

			return true;
		}
	}
}
