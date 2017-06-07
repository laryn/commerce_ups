<?php

/**
 * @file
 * Handles XML-related stuff for Commerce UPS module.
 */

/**
 * Builds the XML access request.
 */
function commerce_ups_build_access_request($order) {
  $api_vars = commerce_ups_decrypt_vars(TRUE);

  $access_request = new SimpleXMLElement('<AccessRequest/>');
  $access_request->AccessLicenseNumber = $api_vars['ups_accesskey'];
  $access_request->UserId = $api_vars['ups_userid'];
  $access_request->Password = $api_vars['ups_password'];

  // Allow other modules to alter the access request.
  drupal_alter('commerce_ups_build_access_request', $access_request, $order);

  return $access_request->asXML();
}

/**
 * This builds the XML to submit to UPS for rates.
 *
 * Here's a decent sample of what the resulting XML should look like: http://sameers.me/2011/01/21/ups-rate-request-sample/
 */
function commerce_ups_build_rate_request($order) {
  $api_vars = commerce_ups_decrypt_vars(TRUE);

  $order_wrapper = entity_metadata_wrapper('commerce_order', $order);

  // Determine the shipping profile reference field name for the order.
  $field_name = commerce_physical_order_shipping_field_name($order);
  $shipping_profile = $order_wrapper->{$field_name}->value();

  // Prepare the shipping address for use in the request.
  if (!empty($order_wrapper->{$field_name}->commerce_customer_address)) {
    $shipping_address = $order_wrapper->{$field_name}->commerce_customer_address->value();
  }
  else {
    $field = field_info_field($field_name);
    $instance = field_info_instance('commerce_customer_profile', 'commerce_customer_address', 'shipping');
    $shipping_address = addressfield_default_values($field, $instance);
  }

  $ups_pickupschedule = variable_get('commerce_ups_pick_up_schedule');

  $packages = commerce_packing_get_packages($order);
  
  /* Pickup Schedule */

  $schedule_code = variable_get('commerce_ups_pick_up_schedule');

  /* Ship To - Customer Shipping Address */

  $order_wrapper = entity_metadata_wrapper('commerce_order', $order);
  // Prepare the shipping address for use in the request.
  if (!empty($order_wrapper->commerce_customer_shipping->commerce_customer_address)) {
    $shipping_address = $order_wrapper->commerce_customer_shipping->commerce_customer_address->value();
  }

  $rating_request = new SimpleXMLElement('<RatingServiceSelectionRequest/>');

  $request = $rating_request->addChild('Request');
  $transaction_reference = $request->addChild('TransactionReference');
  $transaction_reference->CustomerContext = 'Bare Bones Rate Request';
  $transaction_reference->XpciVersion = '1.0001';
  $request->RequestAction = 'Rate';
  $request->RequestOption = 'Shop';
  $request->pickupType->code = $schedule_code;

  $shipment = $rating_request->addChild('Shipment');

  if (variable_get('commerce_ups_negotiated_rates', FALSE)) {
    $rate_information = $shipment->addChild('RateInformation');
    $rate_information->addChild('NegotiatedRatesIndicator');
  }

  $shipper = $shipment->addChild('Shipper');
  $shipper_address = $shipper->addChild('Address');
  $shipper_address->PostalCode = variable_get('commerce_ups_postal_code', '');
  $shipper_address->CountryCode = variable_get('commerce_ups_country_code', '');
  $shipper->ShipperNumber = $api_vars['ups_accountid'];

  $shipto = $shipment->addChild('ShipTo');
  $shipto_address = $shipto->addChild('Address');
  $shipto_address->StateProvinceCode = $shipping_address['administrative_area'];
  $shipto_address->PostalCode = $shipping_address['postal_code'];
  $shipto_address->CountryCode = $shipping_address['country'];
  if (variable_get('commerce_ups_shipto_residential', FALSE)) {
    $shipto_address->ResidentialAddressIndicator = 'true';
  }

  $shipfrom = $shipment->addChild('ShipFrom');
  $shipfrom_address = $shipfrom->addChild('Address');
  $shipfrom_address->StateProvinceCode = variable_get('commerce_ups_state', '');
  $shipfrom_address->PostalCode = variable_get('commerce_ups_postal_code', '');
  $shipfrom_address->CountryCode = variable_get('commerce_ups_country_code', '');
  $shipfrom->ResidentialAddressIndicator = '';

  // Loop through the packages that were previously calculated & add them to the request.
  foreach($packages AS $current_package) {  
    $package = $shipment->addChild('Package');
    $package->PackagingType->Code = variable_get('commerce_ups_packaging', '02');
    $dimensions = $package->addChild('Dimensions');
    $dimensions->UnitOfMeasurement->Code = 'IN';
    $dimensions->addChild('Length', round($current_package['dimensions']['length']));
    $dimensions->addChild('Width', round($current_package['dimensions']['width']));
    $dimensions->addChild('Height', round($current_package['dimensions']['height']));
    $package_weight = $package->addChild('PackageWeight');
    $package_weight->UnitOfMeasurement->Code = 'LBS';
    $package_weight->addChild('Weight', $current_package['weight']['weight']);
  }

  // Allow other modules to alter the rate request.
  drupal_alter('commerce_ups_build_rate_request', $rating_request, $order);

  // Prepend the access request XML.
  $xml = commerce_ups_build_access_request($order) . $rating_request->asXML();

  return $xml;
}

/**
 * Submits an API request to the Progistics XML Processor.
 *
 * @param $method
 *   A string value of the API method that is used to look up the endpoint.
 * @param $xml
 *   An XML string to submit to the Progistics XML Processor.
 * @param $message
 *   Optional log message to use for logged API requests.
 *
 * @return mixed
 *   False if failed, SimpleXMLElement object if successful.
 */
function commerce_ups_api_request($method, $xml, $message = '') {
  // Log the API request if specified.
  if (in_array('request', variable_get('commerce_ups_log', array()))) {
    if (empty($message)) {
      $message = t('Submitting API request to the UPS');
    }
    watchdog('ups', '@message:<pre>@xml</pre>', array('@message' => $message, '@xml' => $xml));
  }

  // Determine the API endpoint for the method.
  $endpoint = commerce_ups_api_endpoint($method);

  // A valid endpoint is required to continue.
  if (empty($endpoint)) {
    watchdog('ups', 'Undefined endpoint for : @method', array('@method' => $method), WATCHDOG_ERROR);
    return FALSE;
  }

  $ch = curl_init($endpoint);
  curl_setopt($ch, CURLOPT_HEADER, 0);
  curl_setopt($ch, CURLOPT_POST, 1);
  curl_setopt($ch, CURLOPT_TIMEOUT, 60);
  curl_setopt($ch, CURLOPT_RETURNTRANSFER, 1);
  curl_setopt($ch, CURLOPT_SSL_VERIFYPEER, 0);
  curl_setopt($ch, CURLOPT_SSL_VERIFYHOST, 0);
  curl_setopt($ch, CURLOPT_POSTFIELDS, $xml);

  $result = curl_exec($ch);

  // Log any errors to the watchdog.
  if ($error = curl_error($ch)) {
    watchdog('ups', 'cURL error: @error', array('@error' => $error), WATCHDOG_ERROR);
    return FALSE;
  }
  curl_close($ch);

  // If we received data back from the server...
  if (!empty($result)) {

    // Extract the result into an XML response object.
    try {
      $response = new SimpleXMLElement($result);
    }
    catch (Exception $ex) {
      watchdog('ups', 'Unable to parse response from UPS as XML: Response <pre>@response</pre>', array('@response' => print_r($result, TRUE)), WATCHDOG_WARNING);
      return FALSE;
    }

    // Log the API request if specified.
    if (in_array('response', variable_get('commerce_ups_log', array()))) {
      watchdog('ups', 'API response received:<pre>@xml</pre>', array('@xml' => $response->asXML()));
    }
    return $response;
  }
  else {
    return FALSE;
  }
}
