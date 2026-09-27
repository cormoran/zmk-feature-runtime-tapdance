/** Real Web Serial transport and protobuf RPC against a Renode firmware DUT.
 * Run: west zmk-web-e2e --elf build/web_e2e/zephyr/zmk.elf -- npm --prefix web run e2e
 */
import { test, expect } from "@playwright/test";
const SHIM_URL = process.env.ZMK_WEB_E2E_SHIM_URL;
const DEVICE_NAME = process.env.ZMK_WEB_E2E_DEVICE_NAME || "Module Test";
test("configures and removes tap dances through real firmware", async ({
  page,
  request,
}) => {
  test.skip(!SHIM_URL, "run through west zmk-web-e2e with a Renode DUT");
  await page.addInitScript(await (await request.get(SHIM_URL!)).text());
  await page.goto("/");
  await page.getByRole("button", { name: /Connect USB/ }).click();
  await expect(page.getByText(`Connected to: ${DEVICE_NAME}`)).toBeVisible();
  await expect(
    page.getByRole("heading", { name: "Runtime tap dances" })
  ).toBeVisible();
  await expect(page.getByText("0 / 16 tap dances")).toBeVisible();
  await page.getByLabel("Tap interval (ms)").fill("250");
  await page.getByRole("button", { name: "Apply interval" }).click();
  await expect(page.getByText(/Applied in RAM/)).toBeVisible();
  await page.getByRole("button", { name: "Save interval" }).click();
  await expect(page.getByText("Saved to flash.")).toBeVisible();
  await page.getByRole("button", { name: "Add tap dance" }).click();
  await page.getByLabel("Position 1").fill("0");
  await page.getByLabel("Timing 1").selectOption("delayed");
  const doublePicker = page.getByLabel("Double 1 behavior", { exact: true });
  const keyPress = doublePicker
    .locator("option")
    .filter({ hasText: /^Key Press/ });
  await expect(keyPress).toHaveCount(1);
  const keyPressId = await keyPress.getAttribute("value");
  await doublePicker.selectOption(keyPressId!);
  await page.getByLabel("Double 1 parameter 1").fill("458756");
  await page
    .getByLabel("Triple 1 behavior", { exact: true })
    .selectOption(keyPressId!);
  await page.getByLabel("Triple 1 parameter 1").fill("458757");
  await page.getByRole("button", { name: "Save tap dance 1" }).click();
  await expect(page.getByText("Saved to flash.")).toBeVisible();
  await page.getByRole("button", { name: "Reload from keyboard" }).click();
  await expect(page.getByLabel("Position 1")).toHaveValue("0");
  await expect(page.getByLabel("Timing 1")).toHaveValue("delayed");
  await expect(
    page.getByLabel("Double 1 behavior", { exact: true })
  ).toHaveValue(keyPressId!);
  await expect(page.getByLabel("Double 1 parameter 1")).toHaveValue("458756");
  await expect(page.getByLabel("Triple 1 parameter 1")).toHaveValue("458757");
  await expect(page.getByLabel("Tap interval (ms)")).toHaveValue("250");
  await page
    .getByRole("button", { name: "Delete tap dance 1 (flash)" })
    .click();
  await expect(page.getByText("0 / 16 tap dances")).toBeVisible();
  await page.getByLabel("Tap interval (ms)").fill("0");
  await page.getByRole("button", { name: "Apply interval" }).click();
  await expect(page.getByRole("alert")).toContainText("1 to 2000");
});
