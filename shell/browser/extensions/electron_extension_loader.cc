// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shell/browser/extensions/electron_extension_loader.h"

#include <utility>

#include "base/auto_reset.h"
#include "base/bind.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/logging.h"
#include "base/sequenced_task_runner.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task_runner_util.h"
#include "base/threading/thread_restrictions.h"
#include "extensions/browser/extension_file_task_runner.h"
#include "extensions/browser/extension_prefs.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/common/file_util.h"

namespace extensions {

using LoadErrorBehavior = ExtensionRegistrar::LoadErrorBehavior;

namespace {

std::pair<scoped_refptr<const Extension>, std::string> LoadUnpacked(
    const base::FilePath& extension_dir,
    int load_flags) {
  // app_shell only supports unpacked extensions.
  // NOTE: If you add packed extension support consider removing the flag
  // FOLLOW_SYMLINKS_ANYWHERE below. Packed extensions should not have symlinks.
  if (!base::DirectoryExists(extension_dir)) {
    std::string err = "Extension directory not found: " +
                      base::UTF16ToUTF8(extension_dir.LossyDisplayName());
    return std::make_pair(nullptr, err);
  }

  std::string load_error;
  scoped_refptr<Extension> extension = file_util::LoadExtension(
      extension_dir, Manifest::COMMAND_LINE, load_flags, &load_error);
  if (!extension.get()) {
    std::string err = "Loading extension at " +
                      base::UTF16ToUTF8(extension_dir.LossyDisplayName()) +
                      " failed with: " + load_error;
    return std::make_pair(nullptr, err);
  }

  std::string warnings;
  // Log warnings.
  if (extension->install_warnings().size()) {
    warnings += "Warnings loading extension at " +
                base::UTF16ToUTF8(extension_dir.LossyDisplayName()) + ":\n";
    for (const auto& warning : extension->install_warnings()) {
      warnings += "  " + warning.message + "\n";
    }
  }

  return std::make_pair(extension, warnings);
}

}  // namespace

ElectronExtensionLoader::ElectronExtensionLoader(
    content::BrowserContext* browser_context)
    : browser_context_(browser_context),
      extension_registrar_(browser_context, this),
      weak_factory_(this) {}

ElectronExtensionLoader::~ElectronExtensionLoader() = default;

void ElectronExtensionLoader::LoadExtension(
    const base::FilePath& extension_dir,
    int load_flags,
    base::OnceCallback<void(const Extension*, const std::string&)> cb) {
  base::PostTaskAndReplyWithResult(
      GetExtensionFileTaskRunner().get(), FROM_HERE,
      base::BindOnce(&LoadUnpacked, extension_dir, load_flags),
      base::BindOnce(&ElectronExtensionLoader::FinishExtensionLoad,
                     weak_factory_.GetWeakPtr(), std::move(cb)));
}

void ElectronExtensionLoader::ReloadExtension(const ExtensionId& extension_id) {
  const Extension* extension = ExtensionRegistry::Get(browser_context_)
                                   ->GetInstalledExtension(extension_id);
  // We shouldn't be trying to reload extensions that haven't been added.
  DCHECK(extension);

  // This should always start false since it's only set here, or in
  // LoadExtensionForReload() as a result of the call below.
  DCHECK_EQ(false, did_schedule_reload_);
  base::AutoReset<bool> reset_did_schedule_reload(&did_schedule_reload_, false);

  extension_registrar_.ReloadExtension(extension_id, LoadErrorBehavior::kQuiet);
  if (did_schedule_reload_)
    return;
}

void ElectronExtensionLoader::UnloadExtension(
    const ExtensionId& extension_id,
    extensions::UnloadedExtensionReason reason) {
  extension_registrar_.RemoveExtension(extension_id, reason);
}

void ElectronExtensionLoader::FinishExtensionLoad(
    base::OnceCallback<void(const Extension*, const std::string&)> cb,
    std::pair<scoped_refptr<const Extension>, std::string> result) {
  scoped_refptr<const Extension> extension = result.first;
  if (extension) {
    extension_registrar_.AddExtension(extension);
  }
  std::move(cb).Run(extension.get(), result.second);
}

void ElectronExtensionLoader::FinishExtensionReload(
    const ExtensionId& old_extension_id,
    std::pair<scoped_refptr<const Extension>, std::string> result) {
  scoped_refptr<const Extension> extension = result.first;
  if (extension) {
    extension_registrar_.AddExtension(extension);
  }
}

void ElectronExtensionLoader::PreAddExtension(const Extension* extension,
                                              const Extension* old_extension) {
  if (old_extension)
    return;

  // The extension might be disabled if a previous reload attempt failed. In
  // that case, we want to remove that disable reason.
  ExtensionPrefs* extension_prefs = ExtensionPrefs::Get(browser_context_);
  if (extension_prefs->IsExtensionDisabled(extension->id()) &&
      extension_prefs->HasDisableReason(extension->id(),
                                        disable_reason::DISABLE_RELOAD)) {
    extension_prefs->RemoveDisableReason(extension->id(),
                                         disable_reason::DISABLE_RELOAD);
    // Only re-enable the extension if there are no other disable reasons.
    if (extension_prefs->GetDisableReasons(extension->id()) ==
        disable_reason::DISABLE_NONE) {
      extension_prefs->SetExtensionEnabled(extension->id());
    }
  }
}

void ElectronExtensionLoader::PostActivateExtension(
    scoped_refptr<const Extension> extension) {}

void ElectronExtensionLoader::PostDeactivateExtension(
    scoped_refptr<const Extension> extension) {}

void ElectronExtensionLoader::LoadExtensionForReload(
    const ExtensionId& extension_id,
    const base::FilePath& path,
    LoadErrorBehavior load_error_behavior) {
  CHECK(!path.empty());

  // TODO(nornagon): we should save whether file access was granted
  // when loading this extension and retain it here. As is, reloading an
  // extension will cause the file access permission to be dropped.
  int load_flags = Extension::FOLLOW_SYMLINKS_ANYWHERE;
  base::PostTaskAndReplyWithResult(
      GetExtensionFileTaskRunner().get(), FROM_HERE,
      base::BindOnce(&LoadUnpacked, path, load_flags),
      base::BindOnce(&ElectronExtensionLoader::FinishExtensionReload,
                     weak_factory_.GetWeakPtr(), extension_id));
  did_schedule_reload_ = true;
}

bool ElectronExtensionLoader::CanEnableExtension(const Extension* extension) {
  return true;
}

bool ElectronExtensionLoader::CanDisableExtension(const Extension* extension) {
  // Extensions cannot be disabled by the user.
  return false;
}

bool ElectronExtensionLoader::ShouldBlockExtension(const Extension* extension) {
  return false;
}

}  // namespace extensions
