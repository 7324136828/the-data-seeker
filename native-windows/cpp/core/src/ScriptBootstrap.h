#pragma once
inline constexpr const char kScriptBootstrap[] = R"DF_SCRIPT(function runScriptFrame(input) {
  const clone = (value) => JSON.parse(JSON.stringify(value));
  const own = (object, key) => Object.prototype.hasOwnProperty.call(object, key);
  const scopes = Object.fromEntries(["globals", "collection", "environment", "data", "local"].map((name) => [name, Object.assign(Object.create(null), input.scopes[name] || {})]));
  const changes = Object.fromEntries(["globals", "collection", "environment", "local"].map((name) => [name, { set: Object.create(null), unset: [] }]));
  const logs = [];
  const tests = [];
  const request = clone(input.request);
  let sourceName = "";
  const text = (value) => typeof value === "string" ? value : JSON.stringify(value);
  const resolved = (key) => {
    for (const name of ["local", "data", "environment", "collection", "globals"]) if (own(scopes[name], key)) return scopes[name][key];
    return input.dynamic[key];
  };
  const replaceIn = (value) => String(value).replace(/\{\{([^}]+)\}\}/g, (match, key) => {
    const value = resolved(key.trim());
    return value === undefined ? match : text(value);
  });
  const scope = (name, merged = false) => Object.freeze({
    get(key) { const value = merged ? resolved(String(key)) : scopes[name][String(key)]; return value === undefined ? undefined : clone(value); },
    has(key) { return merged ? resolved(String(key)) !== undefined : own(scopes[name], String(key)); },
    set(key, value) {
      if (name === "data") throw new Error("Iteration data is read-only.");
      if ((name === "environment" && !input.environmentEnabled) || (name === "collection" && !input.collectionEnabled)) throw new Error(`Select a ${name === "environment" ? "environment" : "saved collection"} before setting its variables.`);
      key = String(key);
      if (!key || key.length > 256) throw new Error("Variable names must contain 1 to 256 characters.");
      if (typeof value === "bigint") value = String(value);
      if (value === undefined || typeof value === "function") throw new Error("Variable values must be JSON serializable.");
      scopes[name][key] = clone(value);
      changes[name].set[key] = clone(value);
      changes[name].unset = changes[name].unset.filter((item) => item !== key);
    },
    unset(key) {
      if (name === "data") throw new Error("Iteration data is read-only.");
      key = String(key);
      delete scopes[name][key];
      delete changes[name].set[key];
      if (!changes[name].unset.includes(key)) changes[name].unset.push(key);
    },
    replaceIn,
    toObject() { return clone(merged ? { ...scopes.globals, ...scopes.collection, ...scopes.environment, ...scopes.data, ...scopes.local } : scopes[name]); },
  });
  const logger = Object.freeze(Object.fromEntries(["log", "info", "warn", "error"].map((level) => [level, (...values) => {
    if (logs.length < 100) logs.push({ level, source: sourceName, phase: input.phase, message: values.map((value) => text(value) ?? "undefined").join(" ").slice(0, 2000) });
  }])));
  const canonical = (value) => Array.isArray(value) ? value.map(canonical) : value && typeof value === "object" ? Object.fromEntries(Object.keys(value).sort().map((key) => [key, canonical(value[key])])) : value;
  const equal = (left, right) => JSON.stringify(canonical(left)) === JSON.stringify(canonical(right));
  function expect(value, negative = false, deep = false) {
    const assert = (condition, message) => { if (negative ? condition : !condition) throw new Error(message); };
    const chain = {
      equal(expected) { assert(deep ? equal(value, expected) : value === expected, `Expected ${text(value)} ${negative ? "not " : ""}to equal ${text(expected)}`); return chain; },
      eql(expected) { assert(equal(value, expected), `Expected values ${negative ? "not " : ""}to be deeply equal`); return chain; },
      include(expected) { assert(typeof value === "string" || Array.isArray(value) ? value.includes(expected) : value && typeof value === "object" && Object.entries(expected).every(([key, item]) => equal(value[key], item)), "Expected value to include the supplied item"); return chain; },
      property(key, expected) { assert(value != null && own(value, key), `Expected property ${key}`); if (arguments.length > 1) assert(equal(value[key], expected), `Unexpected value for property ${key}`); return expect(value?.[key], negative, deep); },
      a(type) { assert(type === "array" ? Array.isArray(value) : type === "null" ? value === null : typeof value === type, `Expected value to be ${type}`); return chain; },
      above(number) { assert(value > number, `Expected value to be above ${number}`); return chain; },
      below(number) { assert(value < number, `Expected value to be below ${number}`); return chain; },
      oneOf(values) { assert(values.includes(value), "Expected value to be one of the supplied values"); return chain; },
      match(pattern) { assert(pattern.test(value), "Expected value to match the pattern"); return chain; },
      lengthOf(length) { assert(value?.length === length, `Expected length ${length}`); return chain; },
    };
    chain.eq = chain.equals = chain.equal;
    chain.an = chain.a;
    chain.contains = chain.include;
    for (const name of ["to", "be", "have", "and", "that", "is"]) Object.defineProperty(chain, name, { get: () => chain });
    Object.defineProperty(chain, "not", { get: () => expect(value, !negative, deep) });
    Object.defineProperty(chain, "deep", { get: () => expect(value, negative, true) });
    for (const [name, condition] of Object.entries({ true: value === true, false: value === false, ok: Boolean(value), exist: value != null, empty: value != null && (value.length === 0 || Object.keys(value).length === 0) })) Object.defineProperty(chain, name, { get: () => { assert(condition, `Expected assertion '${name}' to hold`); return chain; } });
    return chain;
  }
  const itemsApi = (items, readonly = false, insensitive = true) => {
    const matches = (item, key) => insensitive ? String(item.key).toLowerCase() === String(key).toLowerCase() : item.key === key;
    const mutable = () => { if (readonly) throw new Error("Response headers are read-only."); };
    return Object.freeze({
      get(key) { return items.find((item) => matches(item, key))?.value; },
      has(key) { return items.some((item) => matches(item, key)); },
      all() { return clone(items); },
      add(item) { mutable(); if (typeof item === "string") { const split = item.indexOf(":"); item = { key: item.slice(0, split), value: item.slice(split + 1).trim() }; } if (!item?.key) throw new Error("Specify a header or parameter key."); items.push({ ...clone(item), value: text(item.value) ?? "", enabled: true }); },
      upsert(item) { mutable(); const index = items.findIndex((existing) => matches(existing, item.key)); if (index >= 0) items[index] = { ...items[index], ...clone(item), value: text(item.value) ?? "", enabled: true }; else this.add(item); },
      remove(key) { mutable(); for (let index = items.length - 1; index >= 0; index -= 1) if (matches(items[index], key)) items.splice(index, 1); },
    });
  };
  const headers = Array.isArray(request.headers) ? request.headers : [];
  const params = Array.isArray(request.params) ? request.params : [];
  let body = typeof request.body === "string" ? { type: "raw", content: request.body } : request.body || { type: "none", content: "" };
  let url = String(request.url || "");
  const bodyApi = {
    get raw() { return body.content || ""; },
    set raw(value) { this.update(value); },
    update(value) {
      const content = typeof value === "string" ? value : value?.raw ?? value?.content ?? JSON.stringify(value);
      let type = "raw";
      try { JSON.parse(content); type = "json"; } catch {}
      body = { ...body, type, content };
    },
    toString() { return body.content || ""; },
  };
  const urlApi = { toString: () => url, update(value) { url = String(value); }, query: itemsApi(params, false, false), variables: itemsApi(request.pathParams || [], false, false) };
  const exposed = { ...request, headers: itemsApi(headers), body: bodyApi };
  Object.defineProperty(exposed, "url", { get: () => urlApi, set: (value) => { url = String(value); }, enumerable: true });
  const responseHeaders = input.response?.headers || [];
  const response = input.response ? {
    code: input.response.status_code, status: input.response.status_text,
    responseTime: input.response.latency_ms, responseSize: input.response.size_bytes,
    headers: itemsApi(responseHeaders, true),
    text: () => input.response.body || "",
    json: () => JSON.parse(input.response.body || ""),
    to: { have: { status(expected) { if (input.response.status_code !== expected) throw new Error(`Expected status ${expected}`); } } },
  } : undefined;
  const pm = Object.freeze({
    variables: scope("local", true), environment: scope("environment"), collectionVariables: scope("collection"), globals: scope("globals"), iterationData: scope("data"),
    request: exposed, response, expect,
    info: Object.freeze({ requestName: request.name || "Request", requestId: request.id || "", iteration: input.iteration || 0 }),
    test(name, callback) { let passed = true; let message = "OK"; try { const result = callback(); if (result && typeof result.then === "function") throw new Error("Use synchronous assertions in this sandbox."); } catch (error) { passed = false; message = String(error.message || error).slice(0, 1500); } tests.push({ type: "script", name: String(name), passed, message }); },
  });
  globalThis.setTimeout = globalThis.setInterval = globalThis.clearTimeout = globalThis.clearInterval = undefined;
  for (const script of input.scripts) {
    sourceName = script.name;
    try {
      const returned = new Function("pm", "console", '"use strict";\n' + script.code)(pm, logger);
      if (returned && typeof returned.then === "function") throw new Error("Asynchronous scripts are not supported by this sandbox.");
    } catch (error) {
      if (input.phase === "pre-request") throw new Error(`${sourceName} script failed: ${error.message || error}`);
      const message = `${sourceName} script failed: ${error.message || error}`;
      tests.push({ type: "script", name: sourceName, passed: false, message });
      logger.error(message);
    }
  }
  const output = { ...exposed, url, headers, params, body };
  return JSON.stringify({ request: output, changes, tests, console: logs, local: scopes.local });
}
)DF_SCRIPT";
