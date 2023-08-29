def create_profile(path):
    from selenium import webdriver
    fp = webdriver.FirefoxProfile()
    fp.set_preference("browser.download.folderList", 2)
    fp.set_preference("browser.download.manager.showWhenStarting", False)
    fp.set_preference("browser.download.dir", path)
    fp.set_preference("browser.helperApps.alwaysAsk.force", False)
    fp.set_preference("browser.helperApps.neverAsk.saveToDisk",'application/zip')
    fp.set_preference("bdevtools.console.stdout.content", True)
    fp.update_preferences()
    return fp.path
