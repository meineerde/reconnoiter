<?php

require_once('Reconnoiter_DB.php');
$db = Reconnoiter_DB::getDB();

header('Content-Type: application/json; charset=utf-8');

if($_GET['id']) {
  $row = $db->getGraphByID($_GET['id']);
  $graph = json_decode($row['json'], true);
  $graph['id'] = $row['graphid'];
  print json_encode($graph);
}
else{
  print json_encode(array(
    'type' => 'standard',
  ));
}
?>
