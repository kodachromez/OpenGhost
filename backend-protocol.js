(() => {
'use strict';

// ABP uses named parameters and string or safe-integer IDs; batches are not supported.
const isObject = value => value !== null && typeof value === 'object' && !Array.isArray(value);
const isId = value => typeof value === 'string' || Number.isSafeInteger(value);
const has = (value, key) => Object.prototype.hasOwnProperty.call(value, key);

// null means valid. A null response ID is allowed only for JSON-RPC's uncorrelated errors, never a result/request.
function validate(message) {
 const invalid = { code: -32600, message: 'Invalid JSON-RPC envelope' };
 if (!isObject(message) || message.jsonrpc !== '2.0') return invalid;
 const id = has(message, 'id'), method = has(message, 'method');
 const result = has(message, 'result'), error = has(message, 'error');
 if (method) {
  if (typeof message.method !== 'string' || result || error || (id && !isId(message.id))) return invalid;
  if (has(message, 'params') && !isObject(message.params)) return { code: -32602, message: 'Invalid params: expected an object' };
  return null;
 }
 if (!id || (!isId(message.id) && !(message.id === null && error)) || has(message, 'params') || result === error) return invalid;
 if (result) return message.result === undefined ? invalid : null;
 if (!isObject(message.error) || !Number.isSafeInteger(message.error.code) || typeof message.error.message !== 'string') return invalid;
 return null;
}

const protocol = { validate, isObject, isId, has };
if (typeof module !== 'undefined' && module.exports) module.exports = protocol;
else window.BackendProtocol = protocol;
})();
