// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

(async function() {
  TestRunner.addResult(`Tests that splitting properties when pasting works.\n`);
  await TestRunner.loadModule('elements_test_runner');
  await TestRunner.showPanel('elements');
  await TestRunner.loadHTML(`
      <div id="inspected" style="font-size: 12px">Text</div>
      <div id="other"></div>
    `);

  ElementsTestRunner.selectNodeAndWaitForStyles('inspected', pasteFirstProperty);

  function pasteFirstProperty() {
    TestRunner.addResult('Before pasting:');
    ElementsTestRunner.dumpSelectedElementStyles(true);
    var section = ElementsTestRunner.inlineStyleSection();

    var treeElement = section.addNewBlankProperty(0);
    pasteProperty(treeElement, 'margin-left: 1px', pasteTwoProperties);
  }

  function pasteTwoProperties() {
    TestRunner.addResult('After pasting \'margin-left: 1px\':');
    ElementsTestRunner.dumpSelectedElementStyles(true);

    var treeElement = ElementsTestRunner.inlineStyleSection().addNewBlankProperty(2);
    pasteProperty(treeElement, 'margin-top: 1px; color: red;', pasteOverExistingProperty);
  }

  function pasteOverExistingProperty() {
    TestRunner.addResult('After pasting \'margin-top: 1px; color: red;\':');
    ElementsTestRunner.dumpSelectedElementStyles(true);

    var treeElement = ElementsTestRunner.getElementStylePropertyTreeItem('margin-top');
    pasteProperty(treeElement, 'foo: bar; moo: zoo', dumpAndComplete);
  }

  function dumpAndComplete() {
    TestRunner.addResult('After pasting \'foo: bar; moo: zoo\' over \'margin-top\':');
    ElementsTestRunner.dumpSelectedElementStyles(true);

    TestRunner.completeTest();
  }

  function pasteProperty(treeElement, propertyText, callback) {
    treeElement.nameElement.textContent = propertyText;
    treeElement.startEditing();

    document.execCommand('SelectAll');
    document.execCommand('Copy');
    ElementsTestRunner.waitForStyleApplied(reloadStyles.bind(this, callback));
    document.execCommand('Paste');
  }

  function reloadStyles(callback) {
    ElementsTestRunner.selectNodeAndWaitForStyles('other', otherCallback);

    function otherCallback() {
      ElementsTestRunner.selectNodeAndWaitForStyles('inspected', callback);
    }
  }
})();